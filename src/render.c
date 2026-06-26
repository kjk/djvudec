/* render.c -- page rendering. Milestone 3: JB2 bitonal composite.
 * (color IW44 = milestone 5). */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>

/* INFO rotation flag -> clockwise quarter-turn count (90->3, 180->2, 270->1). */
static int rotation_quarter_turns(int rotation)
{
    if (rotation == 90) return 3;
    if (rotation == 180) return 2;
    if (rotation == 270) return 1;
    return 0;
}

/* Rotate a top-down image clockwise by k quarter-turns (k=1,2,3). Returns a
   new image; frees nothing.
   For 90/270 the source is read column-wise; a naive scan both thrashes cache
   (one line per pixel) and re-derives a multiply per pixel. We tile the
   destination into small squares (keeps the source block resident) and walk the
   source with an incremental pointer (no per-pixel index math), with the k and
   bytes/pixel branches hoisted out of the inner loop. This turns a ~30 ms
   pathological transpose on a multi-MB page into a few ms. */
#define ROT_TILE 32
static djvu_image *image_rotate_cw(djvu_ctx *ctx, djvu_image *src, int k)
{
    int comp = (int)src->format;   /* 1 or 3 bytes/pixel */
    int sw = src->width, sh = src->height;
    int dw = (k == 2) ? sw : sh;
    int dh = (k == 2) ? sh : sw;
    size_t srow = (size_t)sw * comp;
    const uint8_t *S;
    djvu_image *d = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    int bx, by, x, y;
    if (!d) return NULL;
    d->width = dw; d->height = dh; d->format = src->format; d->stride = dw * comp;
    d->data = (uint8_t *)djvu_alloc(ctx, (size_t)dw * dh * comp);
    if (!d->data) { djvu_free(ctx, d); return NULL; }
    S = src->data;
    for (by = 0; by < dh; by += ROT_TILE) {
        int ymax = by + ROT_TILE < dh ? by + ROT_TILE : dh;
        for (bx = 0; bx < dw; bx += ROT_TILE) {
            int xmax = bx + ROT_TILE < dw ? bx + ROT_TILE : dw;
            for (y = by; y < ymax; y++) {
                uint8_t *dp = d->data + ((size_t)y * dw + bx) * comp;
                const uint8_t *sp;
                if (k == 1) { /* 90 CW: sx=y, sy=sh-1-x -> step -srow */
                    sp = S + ((size_t)(sh - 1 - bx) * sw + y) * comp;
                    if (comp == 1)
                        for (x = bx; x < xmax; x++) { *dp++ = *sp; sp -= srow; }
                    else
                        for (x = bx; x < xmax; x++) {
                            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                            dp += 3; sp -= srow;
                        }
                } else if (k == 3) { /* 270 CW: sx=sw-1-y, sy=x -> step +srow */
                    sp = S + ((size_t)bx * sw + (sw - 1 - y)) * comp;
                    if (comp == 1)
                        for (x = bx; x < xmax; x++) { *dp++ = *sp; sp += srow; }
                    else
                        for (x = bx; x < xmax; x++) {
                            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                            dp += 3; sp += srow;
                        }
                } else { /* 180: sx=sw-1-x, sy=sh-1-y -> step -comp */
                    sp = S + ((size_t)(sh - 1 - y) * sw + (sw - 1 - bx)) * comp;
                    if (comp == 1)
                        for (x = bx; x < xmax; x++) { *dp++ = *sp--; }
                    else
                        for (x = bx; x < xmax; x++) {
                            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                            dp += 3; sp -= 3;
                        }
                }
            }
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

typedef struct {
    uint8_t *acc;   /* per output-cell ink coverage count (capped at 255) */
    int sw, sh;     /* output (subsampled) dimensions */
    int h;          /* full-res height (for bottom-up -> top-down flip) */
    int sub;        /* subsample factor */
} bitonal_acc_ctx;

/* Accumulate one ink pixel into its subsampled output cell. */
static void bitonal_accum_ink(void *user, int px, int py)
{
    bitonal_acc_ctx *c = (bitonal_acc_ctx *)user;
    int ty, cx, cy;
    size_t cell;

    if (px < 0 || py < 0 || py >= c->h) return;
    ty = c->h - 1 - py;
    cx = px / c->sub;
    cy = ty / c->sub;
    if (cx >= c->sw || cy >= c->sh) return;
    cell = (size_t)cy * c->sw + cx;
    if (c->acc[cell] < 255) c->acc[cell]++;
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

    /* subsample > 1: visit only ink pixels (paper is skipped) and accumulate
       coverage per output cell, then map coverage -> gray via a LUT. O(ink)
       instead of O(full-res): the old path built and box-filtered a full-res
       bitmap, which was ~5x slower than the subsample==1 stamp. */
    {
        uint8_t *acc;
        bitonal_acc_ctx c;
        unsigned char lut[256];
        int sub2 = subsample * subsample;
        size_t n = (size_t)sw * sh, k;

        if (sub2 > 255) sub2 = 255; /* cap so a full cell maps to pure black */
        acc = (uint8_t *)djvu_alloc(ctx, n);
        if (!acc) { djvu_free(ctx, out->data); djvu_free(ctx, out); return NULL; }
        memset(acc, 0, n);
        for (k = 0; k < 256; k++) {
            int cov = (int)k < sub2 ? (int)k : sub2;
            lut[k] = (unsigned char)(255 - cov * 255 / sub2);
        }
        c.acc = acc; c.sw = sw; c.sh = sh; c.h = img->height; c.sub = subsample;
        for (i = 0; i < img->nblits; i++) {
            jb2_blit *b = &img->blits[i];
            jb2_shape *s = djvu_jb2_get_shape(img, b->shapeno);
            if (s && djvu_bm_has_pixels(&s->bm))
                djvu_bm_visit_ink(&s->bm, b->left, b->bottom,
                                  bitonal_accum_ink, &c);
        }
        for (k = 0; k < n; k++) out->data[k] = lut[acc[k]];
        djvu_free(ctx, acc);
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

    (void)subsample; /* rotation applies at every subsample (tiled, cheap) */
    if (!img) return img;
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
    jb2_image *mask = NULL;
    djvu_image *out = NULL;
    double t0;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    if (subsample < 1) subsample = 1;
    if (t) djvu_render_timings_clear(t);
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;
    type = djvu_page_get_type(doc, page_no);
    info_ok = (djvu_doc_page_info(doc, page_no, &pi) == 0);

    /* JB2 mask: cached at doc open (lazy fill on first use when lazy_iw44). */
    if (type == DJVU_PAGE_BITONAL || type == DJVU_PAGE_COMPOUND) {
        if (!djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL))
            goto done;
        if (t) t0 = djvu_bench_now_ms();
        mask = djvu_doc_jb2_mask(doc, page_no);
        if (t) t->jb2_ms += djvu_bench_now_ms() - t0;
        if (!mask) goto done;
    }

    /* Full-res color composite when BG44 is present (compose_background requires it). */
    if (!out && info_ok && subsample == 1 && !ctx->no_compose &&
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
    return apply_page_rotation(ctx, doc, page_no, out, subsample, t);
}

djvu_image *djvu_page_render(djvu_doc *doc, int page_no, int subsample)
{
    return djvu_page_render_timed(doc, page_no, subsample, NULL);
}

/* Decide a render's output geometry/format without decoding pixels, mirroring
   the branch selection in djvu_page_render_timed. Returns 0 and fills outputs
   on success; -1 if the page would not render. *color marks the RGB24 composite
   path; *rotation is the INFO rotation (applied only at subsample==1). */
static int render_plan(djvu_doc *doc, int page_no, int subsample,
                       int *pw, int *ph, djvu_format *pfmt, int *pcolor,
                       int *protation)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off, sz;
    djvu_page_type type;
    djvu_page_info pi;
    int info_ok, has_bg, has_mask, color, w, h, k;
    djvu_format fmt;

    if (!doc || page_no < 0 || page_no >= doc->npages) return -1;
    if (subsample < 1) subsample = 1;
    form_off = doc->pages[page_no].form_off;
    type = djvu_page_get_type(doc, page_no);
    info_ok = (djvu_doc_page_info(doc, page_no, &pi) == 0);
    if (!info_ok || pi.width <= 0 || pi.height <= 0) return -1;
    has_bg = djvu_form_find_chunk(doc, form_off, "BG44", &sz, NULL) != NULL;
    has_mask = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL) != NULL;

    color = subsample == 1 && !ctx->no_compose &&
            (type == DJVU_PAGE_COMPOUND || type == DJVU_PAGE_PHOTO) && has_bg;

    if (color) {
        w = pi.width; h = pi.height; fmt = DJVU_FORMAT_RGB24;
    } else if (type == DJVU_PAGE_UNKNOWN || has_mask) {
        w = (pi.width + subsample - 1) / subsample;
        h = (pi.height + subsample - 1) / subsample;
        fmt = DJVU_FORMAT_GRAY8;
    } else {
        return -1; /* e.g. a photo page at subsample>1 renders nothing */
    }

    k = rotation_quarter_turns(pi.rotation); /* applied at every subsample */
    if (k == 1 || k == 3) { int tmp = w; w = h; h = tmp; } /* 90/270 swap dims */

    *pw = w; *ph = h; *pfmt = fmt; *pcolor = color; *protation = pi.rotation;
    return 0;
}

int djvu_page_render_info(djvu_doc *doc, int page_no, int subsample,
                          djvu_render_info *info)
{
    int w, h, color, rotation;
    djvu_format fmt;
    if (!info) return -1;
    if (render_plan(doc, page_no, subsample, &w, &h, &fmt, &color, &rotation) != 0)
        return -1;
    info->width = w;
    info->height = h;
    info->format = fmt;
    return 0;
}

/* Copy a freshly rendered image into the caller buffer (validates it matches
   the plan). Used for the non-zero-copy cases (gray, rotated, subsampled). */
static int blit_image_into(djvu_image *img, uint8_t *dst, int stride,
                           int w, int h, djvu_format fmt)
{
    int comp = (fmt == DJVU_FORMAT_GRAY8) ? 1 : 3;
    size_t rowbytes = (size_t)w * comp;
    int y;
    if (img->width != w || img->height != h || img->format != fmt) return -1;
    for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * stride, img->data + (size_t)y * img->stride, rowbytes);
    return 0;
}

int djvu_page_render_into(djvu_doc *doc, int page_no, int subsample,
                          uint8_t *dst, int stride)
{
    djvu_ctx *ctx;
    int w, h, color, rotation, k, rc;
    djvu_format fmt;
    djvu_image *img;

    if (!dst) return -1;
    if (render_plan(doc, page_no, subsample, &w, &h, &fmt, &color, &rotation) != 0)
        return -1;
    if (subsample < 1) subsample = 1;
    ctx = doc->ctx;
    k = rotation_quarter_turns(rotation); /* applied at every subsample */

    /* Zero-copy color path: compose straight into dst when no rotation is
       needed (w,h are then the unrotated page dims). */
    if (color && k == 0) {
        uint32_t form_off = doc->pages[page_no].form_off;
        uint32_t sz;
        jb2_image *mask = NULL;

        if (djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL)) {
            mask = djvu_doc_jb2_mask(doc, page_no);
            if (!mask)
                return -1;
        }
        return djvu_compose_page_into(doc, page_no, mask, w, h, dst, stride);
    }

    /* Fallback: render normally (handles gray, rotation, subsampling) then copy
       once into dst. These are the cheap / rare cases. */
    img = djvu_page_render(doc, page_no, subsample);
    if (!img) return -1;
    rc = blit_image_into(img, dst, stride, w, h, fmt);
    djvu_image_destroy(ctx, img);
    return rc;
}

void djvu_image_destroy(djvu_ctx *ctx, djvu_image *img)
{
    if (img) {
        djvu_free(ctx, img->data);
        djvu_free(ctx, img);
    }
}