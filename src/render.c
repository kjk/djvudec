/* render.c -- page rendering. Milestone 3: JB2 bitonal composite.
 * (color IW44 = milestone 5). */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>

/* Borrow or decode the JB2 shape dictionary for a page. Inline and INCL-referenced
   Djbz dicts are cached at doc open; *owned is 0 for those (do not free). */
static jb2_image *load_page_dict(djvu_doc *doc, uint32_t form_off, int *owned)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t sz;
    const uint8_t *djbz;
    jb2_image *dict;

    if (owned) *owned = 0;
    dict = djvu_doc_jb2_dict_inline(doc, form_off);
    if (dict) return dict;
    djbz = djvu_form_find_chunk(doc, form_off, "Djbz", &sz, NULL);
    if (djbz) {
        if (owned) *owned = 1;
        return djvu_jb2_decode_dict(ctx, djbz, sz);
    }
    dict = djvu_doc_jb2_dict_for_form(doc, form_off);
    if (dict) return dict;
    djbz = djvu_form_find_incl_chunk(doc, form_off, "Djbz", &sz);
    if (!djbz) return NULL;
    if (owned) *owned = 1;
    return djvu_jb2_decode_dict(ctx, djbz, sz);
}

/* INFO rotation flag -> clockwise quarter-turn count (90->3, 180->2, 270->1). */
static int rotation_quarter_turns(int rotation)
{
    if (rotation == 90) return 3;
    if (rotation == 180) return 2;
    if (rotation == 270) return 1;
    return 0;
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

/* Solid white gray8 page at INFO dimensions (blank / INFO-only pages). */
static djvu_image *render_blank(djvu_ctx *ctx, const djvu_page_info *pi, int subsample)
{
    djvu_image *out;
    int sw, sh;

    if (pi->width <= 0 || pi->height <= 0) return NULL;
    sw = (pi->width + subsample - 1) / subsample;
    sh = (pi->height + subsample - 1) / subsample;
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) return NULL;
    out->width = sw;
    out->height = sh;
    out->format = DJVU_FORMAT_GRAY8;
    out->stride = sw;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)sw * sh);
    if (!out->data) { djvu_free(ctx, out); return NULL; }
    memset(out->data, 255, (size_t)sw * sh);
    return out;
}

typedef struct {
    uint8_t *dst;
    int w, h;
} bitonal_stamp_ctx;

/* Stamp one ink pixel into top-down gray8 (py is bottom-up page Y). */
static void bitonal_stamp_ink(void *user, int px, int py)
{
    bitonal_stamp_ctx *c = (bitonal_stamp_ctx *)user;
    int ty;

    if (px < 0 || px >= c->w || py < 0 || py >= c->h) return;
    ty = c->h - 1 - py;
    c->dst[(size_t)ty * (size_t)c->w + (size_t)px] = 0;
}

/* Rasterize a decoded JB2 mask to top-down gray8 (ink=0, paper=255). */
static djvu_image *render_bitonal(djvu_ctx *ctx, jb2_image *img, int subsample)
{
    djvu_image *out;
    int sw, sh, i;

    sw = (img->width + subsample - 1) / subsample;
    sh = (img->height + subsample - 1) / subsample;
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) return NULL;
    out->width = sw;
    out->height = sh;
    out->format = DJVU_FORMAT_GRAY8;
    out->stride = sw;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)sw * sh);
    if (!out->data) { djvu_free(ctx, out); return NULL; }

    if (subsample == 1) {
        bitonal_stamp_ctx stamp = { out->data, img->width, img->height };
        memset(out->data, 255, (size_t)sw * sh);
        for (i = 0; i < img->nblits; i++) {
            jb2_blit *b = &img->blits[i];
            jb2_shape *s = djvu_jb2_get_shape(img, b->shapeno);
            if (s && djvu_bm_has_pixels(&s->bm))
                djvu_bm_visit_ink(&s->bm, b->left, b->bottom,
                                  bitonal_stamp_ink, &stamp);
        }
        return out;
    }

    /* subsample > 1: accumulate full-res mask, then box-filter down */
    {
        djvu_bitmap page;
        int y, x;

        memset(&page, 0, sizeof(page));
        if (djvu_bm_init(ctx, &page, img->height, img->width, 0) != 0) {
            djvu_free(ctx, out->data);
            djvu_free(ctx, out);
            return NULL;
        }
        for (i = 0; i < img->nblits; i++) {
            jb2_blit *b = &img->blits[i];
            jb2_shape *s = djvu_jb2_get_shape(img, b->shapeno);
            if (s && djvu_bm_has_pixels(&s->bm))
                djvu_bm_blit(&page, &s->bm, b->left, b->bottom, 1);
        }
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
        djvu_bm_free(ctx, &page);
    }
    return out;
}

static djvu_image *apply_page_rotation(djvu_ctx *ctx, djvu_doc *doc, int page_no,
                                       djvu_image *img, int subsample,
                                       djvu_render_timings *t)
{
    djvu_page_info pi;
    int k;
    djvu_image *r;
    double t0;

    if (!img || subsample != 1) return img;
    if (djvu_doc_page_info(doc, page_no, &pi) != 0 || pi.rotation == 0) return img;
    k = rotation_quarter_turns(pi.rotation);
    if (!k) return img;
    if (t) t0 = djvu_bench_now_ms();
    r = image_rotate_cw(ctx, img, k);
    if (t) t->rotate_ms += djvu_bench_now_ms() - t0;
    if (r) { djvu_image_destroy(ctx, img); return r; }
    return img;
}

djvu_image *djvu_page_render_timed(djvu_doc *doc, int page_no, int subsample,
                                   djvu_render_timings *t)
{
    djvu_ctx *ctx;
    uint32_t form_off, sz;
    djvu_page_type type;
    djvu_page_info pi;
    int info_ok;
    jb2_image *dict = NULL, *mask = NULL;
    int dict_owned = 0;
    djvu_image *out = NULL;
    double t0;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    if (subsample < 1) subsample = 1;
    if (t) djvu_render_timings_clear(t);
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;
    type = djvu_page_get_type(doc, page_no);
    info_ok = (djvu_doc_page_info(doc, page_no, &pi) == 0);

    /* Decode the JB2 mask when the page has one. */
    if (type == DJVU_PAGE_BITONAL || type == DJVU_PAGE_COMPOUND) {
        const uint8_t *sjbz = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL);
        if (!sjbz) goto done;
        if (t) t0 = djvu_bench_now_ms();
        dict = load_page_dict(doc, form_off, &dict_owned);
        mask = djvu_jb2_decode(ctx, sjbz, sz, dict);
        if (t) t->jb2_ms += djvu_bench_now_ms() - t0;
        if (!mask) goto done;
    }

    /* Full-res color composite when BG44 is present (compose_background requires it). */
    if (!out && info_ok && subsample == 1 && !getenv("DJVU_NOCOMPOSE") &&
        (type == DJVU_PAGE_COMPOUND || type == DJVU_PAGE_PHOTO) &&
        djvu_form_find_chunk(doc, form_off, "BG44", &sz, NULL) != NULL) {
        out = djvu_compose_page(doc, page_no, mask, pi.width, pi.height, t);
        if (out) goto done;
    }

    if (!out && type == DJVU_PAGE_UNKNOWN) {
        if (info_ok)
            out = render_blank(ctx, &pi, subsample);
        else
            djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "page %d: nothing to render", page_no);
        goto done;
    }

    if (!out && mask) {
        if (t) t0 = djvu_bench_now_ms();
        out = render_bitonal(ctx, mask, subsample);
        if (t) t->composite_ms += djvu_bench_now_ms() - t0;
    }

done:
    djvu_jb2_free(ctx, mask);
    if (dict_owned) djvu_jb2_free(ctx, dict);
    return apply_page_rotation(ctx, doc, page_no, out, subsample, t);
}

djvu_image *djvu_page_render(djvu_doc *doc, int page_no, int subsample)
{
    return djvu_page_render_timed(doc, page_no, subsample, NULL);
}

void djvu_image_destroy(djvu_ctx *ctx, djvu_image *img)
{
    if (img) {
        djvu_free(ctx, img->data);
        djvu_free(ctx, img);
    }
}