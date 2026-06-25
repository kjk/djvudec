/* render.c -- page rendering. Milestone 3: JB2 bitonal composite.
 * (color IW44 = milestone 5). */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

djvu_image *djvu_compose_page(djvu_doc *doc, int page_no, jb2_image *mask,
                             int width, int height);

/* trim trailing whitespace/control from an INCL id copied into buf */
static void trim_id(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == 0x1a))
        s[--n] = 0;
}

/* Decode the JB2 shape dictionary for a page: either embedded directly in the
   page form as a Djbz chunk, or referenced by an INCL chunk pointing at a
   shared DJVI component that contains the Djbz. */
static jb2_image *load_page_dict(djvu_doc *doc, uint32_t form_off)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t start = 0, sz;
    const uint8_t *incl;
    const uint8_t *djbz_inpage;

    /* in-page Djbz takes precedence */
    djbz_inpage = djvu_form_find_chunk(doc, form_off, "Djbz", &sz, NULL);
    if (djbz_inpage)
        return djvu_jb2_decode_dict(ctx, djbz_inpage, sz);

    while ((incl = djvu_form_find_chunk(doc, form_off, "INCL", &sz, &start)) != NULL) {
        char id[64];
        uint32_t coff, dsz;
        const uint8_t *djbz;
        size_t n = sz < sizeof(id) - 1 ? sz : sizeof(id) - 1;
        memcpy(id, incl, n);
        id[n] = 0;
        trim_id(id);
        coff = djvu_doc_component_offset(doc, id);
        if (!coff) continue;
        djbz = djvu_form_find_chunk(doc, coff, "Djbz", &dsz, NULL);
        if (djbz)
            return djvu_jb2_decode_dict(ctx, djbz, dsz);
    }
    return NULL;
}

/* Rotate a top-down image clockwise by k quarter-turns (k=1,2,3). Returns a
   new image; frees nothing. */
static djvu_image *image_rotate_cw(djvu_ctx *ctx, djvu_image *src, int k)
{
    int comp = (int)src->format;   /* 1 or 3 bytes/pixel */
    int sw = src->width, sh = src->height;
    int dw = (k == 2) ? sw : sh;
    int dh = (k == 2) ? sh : sw;
    djvu_image *d = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    int x, y, c;
    if (!d) return NULL;
    d->width = dw; d->height = dh; d->format = src->format; d->stride = dw * comp;
    d->data = (uint8_t *)djvu_alloc(ctx, (size_t)dw * dh * comp);
    if (!d->data) { djvu_free(ctx, d); return NULL; }
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            int sx, sy;
            if (k == 1) { sx = y; sy = sh - 1 - x; }          /* 90 CW */
            else if (k == 2) { sx = sw - 1 - x; sy = sh - 1 - y; } /* 180 */
            else { sx = sw - 1 - y; sy = x; }                  /* 270 CW */
            for (c = 0; c < comp; c++)
                d->data[((size_t)y * dw + x) * comp + c] =
                    src->data[((size_t)sy * sw + sx) * comp + c];
        }
    }
    return d;
}

djvu_image *djvu_page_render(djvu_doc *doc, int page_no, int subsample)
{
    djvu_ctx *ctx;
    djvu_page_int *pg;
    uint32_t form_off, sz;
    const uint8_t *sjbz;
    jb2_image *dict = NULL, *img = NULL;
    djvu_bitmap page;
    djvu_image *out = NULL;
    int i, y, x;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    if (subsample < 1) subsample = 1;
    ctx = doc->ctx;
    pg = &doc->pages[page_no];
    form_off = pg->form_off;

    {
        uint32_t cs;
        int has_bg = djvu_form_find_chunk(doc, form_off, "BG44", &cs, NULL) != NULL;
        int has_fg = djvu_form_find_chunk(doc, form_off, "FG44", &cs, NULL) != NULL;

        sjbz = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL);
        if (sjbz) {
            dict = load_page_dict(doc, form_off);
            img = djvu_jb2_decode(ctx, sjbz, sz, dict);
            if (!img) goto done;
        }

        /* color/gray page: composite (background + optional mask/foreground).
           Works with img == NULL too (pure-photo pages = BG44 only). */
        if ((has_bg || has_fg) && subsample == 1 && !getenv("DJVU_NOCOMPOSE")) {
            djvu_page_info pi;
            if (djvu_doc_page_info(doc, page_no, &pi) == 0) {
                out = djvu_compose_page(doc, page_no, img, pi.width, pi.height);
                goto done;
            }
        }

        if (!sjbz) {
            /* blank page (only INFO, no image layers): render solid white */
            djvu_page_info pi;
            if (djvu_doc_page_info(doc, page_no, &pi) == 0 &&
                pi.width > 0 && pi.height > 0) {
                int sw = (pi.width + subsample - 1) / subsample;
                int sh = (pi.height + subsample - 1) / subsample;
                out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
                if (out) {
                    out->width = sw; out->height = sh;
                    out->format = DJVU_FORMAT_GRAY8; out->stride = sw;
                    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)sw * sh);
                    if (out->data) memset(out->data, 255, (size_t)sw * sh);
                    else { djvu_free(ctx, out); out = NULL; }
                }
            } else {
                djvu_errorf(ctx, DJVU_SEVERITY_ERROR,
                            "page %d: nothing to render", page_no);
            }
            goto done;
        }
    }

    /* compose mask into a full-res bottom-up bitmap (Grays=2 -> clamp 1) */
    memset(&page, 0, sizeof(page));
    if (djvu_bm_init(ctx, &page, img->height, img->width, 0) != 0) goto done;
    for (i = 0; i < img->nblits; i++) {
        jb2_blit *b = &img->blits[i];
        jb2_shape *s = djvu_jb2_get_shape(img, b->shapeno);
        if (s && s->bm.data)
            djvu_bm_blit(&page, &s->bm, b->left, b->bottom, 1);
    }

    /* emit gray8, top-down, ink(1)->0 white(0)->255, with box subsampling */
    {
        int sw = (img->width + subsample - 1) / subsample;
        int sh = (img->height + subsample - 1) / subsample;
        out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
        if (!out) { djvu_bm_free(ctx, &page); goto done; }
        out->width = sw; out->height = sh;
        out->format = DJVU_FORMAT_GRAY8;
        out->stride = sw;
        out->data = (uint8_t *)djvu_alloc(ctx, (size_t)sw * sh);
        if (!out->data) { djvu_free(ctx, out); out = NULL; djvu_bm_free(ctx, &page); goto done; }

        if (subsample == 1) {
            for (y = 0; y < img->height; y++) {
                /* page row (img->height-1-y) is top row y (flip) */
                int srow = djvu_bm_rowoffset(&page, img->height - 1 - y);
                uint8_t *dst = out->data + (size_t)y * sw;
                for (x = 0; x < img->width; x++)
                    dst[x] = page.data[srow + x] ? 0 : 255;
            }
        } else {
            for (y = 0; y < sh; y++) {
                uint8_t *dst = out->data + (size_t)y * sw;
                for (x = 0; x < sw; x++) {
                    int cnt = 0, tot = 0, yy, xx;
                    for (yy = 0; yy < subsample; yy++) {
                        int py = y * subsample + yy;
                        int srow;
                        if (py >= img->height) break;
                        srow = djvu_bm_rowoffset(&page, img->height - 1 - py);
                        for (xx = 0; xx < subsample; xx++) {
                            int px = x * subsample + xx;
                            if (px >= img->width) continue;
                            tot++;
                            if (page.data[srow + px]) cnt++;
                        }
                    }
                    dst[x] = tot ? (uint8_t)(255 - (cnt * 255 / tot)) : 255;
                }
            }
        }
    }
    djvu_bm_free(ctx, &page);

done:
    djvu_jb2_free(ctx, img);
    djvu_jb2_free(ctx, dict);
    /* apply page rotation (INFO flag) to match ddjvu's upright display */
    if (out && subsample == 1) {
        djvu_page_info pi;
        if (djvu_doc_page_info(doc, page_no, &pi) == 0 && pi.rotation != 0) {
            int k = (pi.rotation == 90) ? 3 : (pi.rotation == 180) ? 2 : 1;
            djvu_image *r = image_rotate_cw(ctx, out, k);
            if (r) { djvu_image_destroy(ctx, out); out = r; }
        }
    }
    return out;
}

void djvu_image_destroy(djvu_ctx *ctx, djvu_image *img)
{
    if (img) {
        djvu_free(ctx, img->data);
        djvu_free(ctx, img);
    }
}

/* ---- debug/verification helpers ---- */

/* Decode a page's BG44 (kind=0) or FG44 (kind=1) IW44 chunks via our decoder,
   returning the native-resolution RGB image. */
djvu_image *djvu_debug_render_iw(djvu_doc *doc, int page_no, int kind)
{
    djvu_ctx *ctx;
    uint32_t form_off, start = 0, sz;
    const uint8_t *chunk;
    const char *id = kind ? "FG44" : "BG44";
    iw_pixmap *pm;
    djvu_image *out = NULL;
    int w, h;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;
    pm = djvu_iw44_new(ctx);
    if (!pm) return NULL;
    {
        const char *mc = getenv("DJVU_IW_MAXCHUNKS");
        int maxc = mc ? atoi(mc) : 1000, nc = 0;
        while ((chunk = djvu_form_find_chunk(doc, form_off, id, &sz, &start)) != NULL) {
            if (nc++ >= maxc) break;
            if (djvu_iw44_decode_chunk(pm, chunk, sz) != 0) { djvu_iw44_free(pm); return NULL; }
        }
    }
    w = djvu_iw44_width(pm); h = djvu_iw44_height(pm);
    if (w <= 0 || h <= 0) { djvu_iw44_free(pm); return NULL; }
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_iw44_free(pm); return NULL; }
    out->width = w; out->height = h; out->format = DJVU_FORMAT_RGB24;
    out->stride = w * 3;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)w * h * 3);
    if (!out->data || djvu_iw44_render_rgb(pm, out->data) != 0) {
        djvu_image_destroy(ctx, out); djvu_iw44_free(pm); return NULL;
    }
    djvu_iw44_free(pm);
    return out;
}

/* Render a page's BG44/FG44 Y (luma) plane as gray (Y+128) -- for isolating
   the luma transform from the color conversion. */
djvu_image *djvu_debug_render_iw_gray(djvu_doc *doc, int page_no, int kind)
{
    djvu_ctx *ctx;
    uint32_t form_off, start = 0, sz;
    const uint8_t *chunk;
    const char *id = kind ? "FG44" : "BG44";
    iw_pixmap *pm;
    djvu_image *out = NULL;
    int w, h;
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;
    pm = djvu_iw44_new(ctx);
    if (!pm) return NULL;
    while ((chunk = djvu_form_find_chunk(doc, form_off, id, &sz, &start)) != NULL)
        if (djvu_iw44_decode_chunk(pm, chunk, sz) != 0) { djvu_iw44_free(pm); return NULL; }
    w = djvu_iw44_width(pm); h = djvu_iw44_height(pm);
    if (w <= 0 || h <= 0) { djvu_iw44_free(pm); return NULL; }
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_iw44_free(pm); return NULL; }
    out->width = w; out->height = h; out->format = DJVU_FORMAT_GRAY8; out->stride = w;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)w * h);
    if (!out->data || djvu_iw44_render_gray(pm, out->data) != 0) {
        djvu_image_destroy(ctx, out); djvu_iw44_free(pm); return NULL;
    }
    djvu_iw44_free(pm);
    return out;
}

djvu_image *djvu_debug_render_iw_plane(djvu_doc *doc, int page_no, int kind, int plane)
{
    djvu_ctx *ctx; uint32_t form_off, start = 0, sz; const uint8_t *chunk;
    const char *id = kind ? "FG44" : "BG44"; iw_pixmap *pm; djvu_image *out;
    int w, h;
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx; form_off = doc->pages[page_no].form_off;
    pm = djvu_iw44_new(ctx); if (!pm) return NULL;
    while ((chunk = djvu_form_find_chunk(doc, form_off, id, &sz, &start)) != NULL)
        if (djvu_iw44_decode_chunk(pm, chunk, sz) != 0) { djvu_iw44_free(pm); return NULL; }
    w = djvu_iw44_width(pm); h = djvu_iw44_height(pm);
    if (w <= 0 || h <= 0) { djvu_iw44_free(pm); return NULL; }
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_iw44_free(pm); return NULL; }
    out->width = w; out->height = h; out->format = DJVU_FORMAT_GRAY8; out->stride = w;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)w * h);
    if (!out->data || djvu_iw44_render_plane(pm, plane, out->data) != 0) {
        djvu_image_destroy(ctx, out); djvu_iw44_free(pm); return NULL;
    }
    djvu_iw44_free(pm);
    return out;
}

static void put_u32be(FILE *f, uint32_t v)
{
    fputc((v >> 24) & 0xff, f); fputc((v >> 16) & 0xff, f);
    fputc((v >> 8) & 0xff, f); fputc(v & 0xff, f);
}

/* Write a standalone AT&TFORM:PM44 file from the page's BG44/FG44 chunks
   (renamed to PM44) so DjVuLibre ddjvu can render the same IW44 data. */
int djvu_debug_dump_iw(djvu_doc *doc, int page_no, int kind, const char *path)
{
    uint32_t form_off, start = 0, sz;
    const uint8_t *chunk;
    const char *id = kind ? "FG44" : "BG44";
    FILE *f;
    uint32_t total = 4;  /* "PM44" type */
    const uint8_t *chunks[64]; uint32_t sizes[64]; int n = 0, i;

    if (!doc || page_no < 0 || page_no >= doc->npages) return -1;
    form_off = doc->pages[page_no].form_off;
    {
    const char *mc = getenv("DJVU_IW_MAXCHUNKS");
    int maxc = mc ? atoi(mc) : 1000;
    while ((chunk = djvu_form_find_chunk(doc, form_off, id, &sz, &start)) != NULL && n < 64) {
        if (n >= maxc) break;
        chunks[n] = chunk; sizes[n] = sz; n++;
        total += 8 + sz + (sz & 1);
    }
    }
    if (n == 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("AT&TFORM", 1, 8, f);
    put_u32be(f, total);
    fwrite("PM44", 1, 4, f);
    for (i = 0; i < n; i++) {
        fwrite("PM44", 1, 4, f);
        put_u32be(f, sizes[i]);
        fwrite(chunks[i], 1, sizes[i], f);
        if (sizes[i] & 1) fputc(0, f);
    }
    fclose(f);
    return 0;
}
