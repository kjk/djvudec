/* compose.c -- page compositing: background (IW44, upsampled) + foreground
 * (FG44 or FGbz palette) stenciled through the JB2 mask.
 * Scaler in scaler.c; stencil ported from DjVuLibre GPixmap.cpp / DjVuImage.cpp.
 * Pixmaps are RGB bottom-up; output is flipped to top-down at the end. */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int compose_bg_page_no(djvu_doc *doc, uint32_t form_off)
{
    int i;
    if (!doc) return -1;
    for (i = 0; i < doc->npages; i++)
        if (doc->pages[i].form_off == form_off)
            return i;
    return -1;
}

static int compose_background_from_native(djvu_ctx *ctx, const djvu_cpix *native,
                                          int width, int height, djvu_cpix *out)
{
    int red;

    if (!native || !native->d || native->w <= 0 || native->h <= 0) return -1;
    red = djvu_compute_red(width, height, native->w, native->h);
    if (red < 1) return -1;
    if (red == 1 && native->w == width && native->h == height) {
        size_t n = (size_t)width * (size_t)height * 3;
        if (djvu_cpix_init(ctx, out, width, height) != 0) return -1;
        memcpy(out->d, native->d, n);
        return 0;
    }
    return djvu_cpix_scale(ctx, native, out, width, height, red);
}

static int compose_bg_native_build(djvu_doc *doc, djvu_page_int *pg)
{
    djvu_ctx *ctx = doc->ctx;
    iw_pixmap *pm;
    int bw, bh, w, h, pm_owned = 0;
    uint32_t sz;

    if (!djvu_cache_stores_page(ctx)) return -1;
    if (!doc || !pg || pg->bg_native.d) return 0;
    if (!pg->has_info || pg->info.width <= 0 || pg->info.height <= 0)
        return -1;
    if (!djvu_form_find_chunk(doc, pg->form_off, "BG44", &sz, NULL))
        return -1;
    pm = djvu_doc_iw44_by_form_acquire(doc, pg->form_off, "BG44", &pm_owned);
    if (!pm) return -1;
    bw = djvu_iw44_width(pm);
    bh = djvu_iw44_height(pm);
    if (bw <= 0 || bh <= 0) {
        djvu_doc_iw44_release(ctx, pm, pm_owned);
        return -1;
    }
    if (djvu_cpix_init(ctx, &pg->bg_native, bw, bh) != 0) {
        djvu_doc_iw44_release(ctx, pm, pm_owned);
        return -1;
    }
    if (djvu_iw44_render_rgb_raw(pm, pg->bg_native.d) != 0) {
        djvu_cpix_free(ctx, &pg->bg_native);
        djvu_doc_iw44_release(ctx, pm, pm_owned);
        return -1;
    }
    w = pg->info.width;
    h = pg->info.height;
    if (!pg->bg_scaled.d &&
        compose_background_from_native(ctx, &pg->bg_native, w, h, &pg->bg_scaled) != 0) {
        djvu_cpix_free(ctx, &pg->bg_native);
        djvu_doc_iw44_release(ctx, pm, pm_owned);
        return -1;
    }
    djvu_doc_iw44_release(ctx, pm, pm_owned);
    return 0;
}

void djvu_doc_preload_compose_bg_range(djvu_doc *doc, int lo0, int hi0)
{
    int i;

    if (!doc || !djvu_cache_stores_page(doc->ctx)) return;
    if (lo0 < 0) lo0 = 0;
    if (hi0 >= doc->npages) hi0 = doc->npages - 1;
    if (lo0 > hi0) return;
    for (i = lo0; i <= hi0; i++)
        compose_bg_native_build(doc, &doc->pages[i]);
}

int djvu_compose_background(djvu_doc *doc, uint32_t form_off, int width, int height,
                            djvu_cpix *out)
{
    djvu_ctx *ctx = doc->ctx;
    iw_pixmap *pm;
    int page_no, bw, bh, red, rc = -1, pm_owned = 0;
    djvu_cpix native;
    djvu_page_int *pg;

    memset(&native, 0, sizeof(native));
    page_no = compose_bg_page_no(doc, form_off);
    if (page_no >= 0 && djvu_cache_stores_page(ctx)) {
        pg = &doc->pages[page_no];
        if (!pg->bg_native.d)
            compose_bg_native_build(doc, pg);
        if (pg->bg_scaled.d && pg->bg_scaled.w == width && pg->bg_scaled.h == height) {
            size_t n = (size_t)width * (size_t)height * 3;
            djvu_free(ctx, out->d);
            out->w = width;
            out->h = height;
            out->d = (uint8_t *)djvu_alloc(ctx, n);
            if (!out->d) return -1;
            memcpy(out->d, pg->bg_scaled.d, n);
            return 0;
        }
        if (pg->bg_native.d)
            return compose_background_from_native(ctx, &pg->bg_native, width, height, out);
    }

    pm = djvu_doc_iw44_by_form_acquire(doc, form_off, "BG44", &pm_owned);
    if (!pm) return -1;
    bw = djvu_iw44_width(pm); bh = djvu_iw44_height(pm);
    red = djvu_compute_red(width, height, bw, bh);
    if (red < 1) goto done;
    if (djvu_cpix_init(ctx, &native, bw, bh) != 0) goto done;
    if (djvu_iw44_render_rgb_raw(pm, native.d) != 0) goto done;
    if (red == 1) {
        *out = native; native.d = NULL; rc = 0;
    } else {
        rc = djvu_cpix_scale(ctx, &native, out, width, height, red);
    }
done:
    djvu_cpix_free(ctx, &native);
    djvu_doc_iw44_release(ctx, pm, pm_owned);
    return rc;
}

static int build_gamma_lut(double corr, unsigned char lut[256])
{
    int i;
    if (corr < 0.1) corr = 0.1; else if (corr > 10.0) corr = 10.0;
    if (corr > 0.999 && corr < 1.001) {
        for (i = 0; i < 256; i++) lut[i] = (unsigned char)i;
        return 0;
    }
    for (i = 0; i < 256; i++) {
        double x = pow((double)i / 255.0, 1.0 / corr);
        int v = (int)floor(255.0 * x + 0.5);
        lut[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    lut[0] = 0; lut[255] = 255;
    return 1;
}

static double page_gamma(djvu_doc *doc, uint32_t form_off)
{
    uint32_t sz;
    const uint8_t *info = djvu_form_find_chunk(doc, form_off, "INFO", &sz, NULL);
    if (info && sz >= 9 && info[8] != 0)
        return (double)info[8] / 10.0;
    return 2.2;
}

typedef struct {
    djvu_cpix *bg;
    int palr, palg, palb;
    int has_pal, has_fg;
    int fgred;
    djvu_cpix *fgnat;
} compose_ink_ctx;

static void compose_stamp_ink(void *user, int px, int py)
{
    compose_ink_ctx *ink = (compose_ink_ctx *)user;
    uint8_t *d;

    if (py < 0 || py >= ink->bg->h || px < 0 || px >= ink->bg->w) return;
    d = ink->bg->d + ((size_t)py * ink->bg->w + px) * 3;
    if (ink->has_pal) {
        d[0] = (uint8_t)ink->palr; d[1] = (uint8_t)ink->palg; d[2] = (uint8_t)ink->palb;
    } else if (ink->has_fg) {
        int fx = px / ink->fgred, fy = py / ink->fgred;
        if (fx >= ink->fgnat->w) fx = ink->fgnat->w - 1;
        if (fy >= ink->fgnat->h) fy = ink->fgnat->h - 1;
        {
            uint8_t *f = ink->fgnat->d + ((size_t)fy * ink->fgnat->w + fx) * 3;
            d[0] = f[0]; d[1] = f[1]; d[2] = f[2];
        }
    } else {
        d[0] = d[1] = d[2] = 0;
    }
}

/* Flip the composited bottom-up pixmap *bg into a top-down destination buffer.
   Swaps R<->B when bgr (B,G,R output); applies the gamma LUT when lut != NULL.
   Honors dst stride, so it can write straight into a caller's DIB row. */
static void compose_finalize(uint8_t *dst, int stride, const djvu_cpix *bg,
                             int bgr, const unsigned char *lut)
{
    int x, y;

    if (!lut && !bgr) {
        size_t row = (size_t)bg->w * 3;
        for (y = 0; y < bg->h; y++)
            memcpy(dst + (size_t)y * stride,
                   bg->d + (size_t)(bg->h - 1 - y) * row, row);
        return;
    }

    for (y = 0; y < bg->h; y++) {
        const uint8_t *s = bg->d + (size_t)(bg->h - 1 - y) * bg->w * 3;
        uint8_t *d = dst + (size_t)y * stride;
        for (x = 0; x < bg->w; x++) {
            uint8_t r = s[0], g = s[1], b = s[2];
            if (lut) { r = lut[r]; g = lut[g]; b = lut[b]; }
            if (bgr) { d[0] = b; d[1] = g; d[2] = r; }
            else     { d[0] = r; d[1] = g; d[2] = b; }
            d += 3; s += 3;
        }
    }
}

typedef struct {
    uint8_t *pal;
    int palsize;
    short *colordata;
    int ncolor;
} fgbz_palette;

static void fgbz_palette_free(djvu_ctx *ctx, fgbz_palette *fg)
{
    if (!fg) return;
    djvu_free(ctx, fg->pal);
    djvu_free(ctx, fg->colordata);
    memset(fg, 0, sizeof(*fg));
}

static int fgbz_palette_parse(djvu_ctx *ctx, const uint8_t *fgbz,
                              uint32_t sz, fgbz_palette *fg)
{
    size_t p = 0;
    int version, i;

    memset(fg, 0, sizeof(*fg));
    if (!fgbz || sz < 3) return -1;
    version = fgbz[p++];
    fg->palsize = (fgbz[p] << 8) | fgbz[p + 1];
    p += 2;
    if ((size_t)p + (size_t)fg->palsize * 3 > sz) return -1;
    fg->pal = (uint8_t *)djvu_alloc(ctx, (size_t)fg->palsize * 3);
    if (!fg->pal) return -1;
    memcpy(fg->pal, fgbz + p, (size_t)fg->palsize * 3);
    p += (size_t)fg->palsize * 3;

    if ((version & 0x80) && p + 3 <= sz) {
        int datasize = (fgbz[p] << 16) | (fgbz[p + 1] << 8) | fgbz[p + 2];
        size_t dlen = 0;
        uint8_t *dd;

        p += 3;
        dd = djvu_bzz_decode_all(ctx, fgbz + p, sz - p, &dlen);
        if (dd && (size_t)datasize * 2 <= dlen) {
            fg->colordata = (short *)djvu_alloc(ctx, sizeof(short) * datasize);
            if (fg->colordata) {
                for (i = 0; i < datasize; i++)
                    fg->colordata[i] = (short)((dd[i * 2] << 8) | dd[i * 2 + 1]);
                fg->ncolor = datasize;
            }
        }
        djvu_free(ctx, dd);
    }
    return 0;
}

static int compose_background_topdown_rgb(djvu_doc *doc, uint32_t form_off,
                                          int width, int height,
                                          uint8_t *dst, int stride,
                                          djvu_render_timings *t)
{
    djvu_ctx *ctx = doc->ctx;
    iw_pixmap *pm;
    djvu_cpix native;
    int bw, bh, red, pm_owned = 0, rc = -1;
    double t0 = 0.0;

    memset(&native, 0, sizeof(native));
    if (t) t0 = djvu_bench_now_ms();
    pm = djvu_doc_iw44_by_form_acquire(doc, form_off, "BG44", &pm_owned);
    if (!pm) goto done;
    bw = djvu_iw44_width(pm);
    bh = djvu_iw44_height(pm);
    red = djvu_compute_red(width, height, bw, bh);
    if (red < 1) goto done;
    if (djvu_cpix_init(ctx, &native, bw, bh) != 0) goto done;
    if (djvu_iw44_render_rgb_raw(pm, native.d) != 0) goto done;
    if (red == 1 && bw == width && bh == height) {
        size_t row = (size_t)width * 3;
        int y;
        for (y = 0; y < height; y++)
            memcpy(dst + (size_t)y * stride,
                   native.d + (size_t)(height - 1 - y) * row, row);
        rc = 0;
    } else {
        rc = djvu_cpix_scale_to_topdown_rgb(ctx, &native, dst, stride,
                                            width, height, red);
    }
done:
    if (t) t->iw44_ms += djvu_bench_now_ms() - t0;
    djvu_cpix_free(ctx, &native);
    djvu_doc_iw44_release(ctx, pm, pm_owned);
    return rc;
}

static int compose_read_bm_run(const uint8_t **data)
{
    int z = *(*data)++;
    if (z >= 0xc0)
        z = ((z & ~0xc0) << 8) | (int)(*(*data)++);
    return z;
}

static void compose_fill_rgb_run(uint8_t *d, int n, int r, int g, int b)
{
    while (n-- > 0) {
        d[0] = (uint8_t)r;
        d[1] = (uint8_t)g;
        d[2] = (uint8_t)b;
        d += 3;
    }
}

static void compose_stamp_bitmap_topdown_rgb(const djvu_bitmap *src,
                                             int left, int bottom,
                                             int outw, int outh,
                                             uint8_t *dst, int stride,
                                             int r, int g, int b)
{
    if (!src || outw <= 0 || outh <= 0) return;
    if (src->rle) {
        const uint8_t *runs = src->rle;
        const uint8_t *runs_end = src->rle + src->rle_len;
        int sr = src->height - 1;
        int sc = 0, p = 0;

        while (runs < runs_end && sr >= 0) {
            int z = compose_read_bm_run(&runs);
            int nc;

            if (sc + z > src->width) return;
            nc = sc + z;
            if (p) {
                int py = bottom + sr;
                if (py >= 0 && py < outh) {
                    int x0 = left + sc;
                    int x1 = left + nc;
                    if (x0 < 0) x0 = 0;
                    if (x1 > outw) x1 = outw;
                    if (x0 < x1) {
                        uint8_t *d = dst + (size_t)(outh - 1 - py) * stride
                                   + (size_t)x0 * 3;
                        compose_fill_rgb_run(d, x1 - x0, r, g, b);
                    }
                }
            }
            sc = nc;
            p = 1 - p;
            if (sc >= src->width) {
                sc = 0;
                p = 0;
                sr--;
            }
        }
    } else if (src->data) {
        int rr;
        for (rr = 0; rr < src->height; rr++) {
            int py = bottom + rr;
            int cc;
            const uint8_t *srow;
            if (py < 0 || py >= outh) continue;
            srow = src->data + djvu_bm_rowoffset(src, rr);
            for (cc = 0; cc < src->width; cc++) {
                int px = left + cc;
                if (px >= 0 && px < outw && srow[cc]) {
                    uint8_t *d = dst + (size_t)(outh - 1 - py) * stride
                               + (size_t)px * 3;
                    d[0] = (uint8_t)r;
                    d[1] = (uint8_t)g;
                    d[2] = (uint8_t)b;
                }
            }
        }
    }
}

static int compose_fgbz_stencil_topdown_rgb(jb2_image *mask,
                                            const fgbz_palette *fg,
                                            int width, int height,
                                            uint8_t *dst, int stride)
{
    int i;

    for (i = 0; mask && i < mask->nblits; i++) {
        jb2_blit *b = &mask->blits[i];
        jb2_shape *s = djvu_jb2_get_shape(mask, b->shapeno);
        int r = 0, g = 0, bl = 0;

        if (!s || !djvu_bm_has_pixels(&s->bm)) continue;
        if (fg->pal && fg->colordata && i < fg->ncolor) {
            int ci = fg->colordata[i];
            if (ci >= 0 && ci < fg->palsize) {
                bl = fg->pal[ci * 3 + 0];
                g  = fg->pal[ci * 3 + 1];
                r  = fg->pal[ci * 3 + 2];
            }
        }
        compose_stamp_bitmap_topdown_rgb(&s->bm, b->left, b->bottom,
                                         width, height, dst, stride, r, g, bl);
    }
    return 0;
}

/* Composite a page into *bg (bottom-up RGB; caller frees via djvu_cpix_free).
   Returns 0 on success, -1 on failure. */
static int compose_to_bg(djvu_doc *doc, int page_no, jb2_image *mask,
                         int width, int height, djvu_render_timings *t,
                         djvu_cpix *bgout)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off = doc->pages[page_no].form_off;
    djvu_cpix bg;
    uint32_t sz; const uint8_t *fgbz;
    uint8_t *pal = NULL; int palsize = 0;
    short *colordata = NULL; int ncolor = 0;
    iw_pixmap *fgpm = NULL; djvu_cpix fgnat; int fgred = 0, fg_owned = 0;
    int i;
    double t0 = 0.0;

    memset(&bg, 0, sizeof(bg)); memset(&fgnat, 0, sizeof(fgnat));
    if (t) t0 = djvu_bench_now_ms();
    if (djvu_compose_background(doc, form_off, width, height, &bg) != 0)
        return -1;
    if (t) t->iw44_ms += djvu_bench_now_ms() - t0;

    if (t) t0 = djvu_bench_now_ms();

    fgbz = djvu_form_find_chunk(doc, form_off, "FGbz", &sz, NULL);
    if (fgbz && sz >= 3) {
        size_t p = 0;
        int version = fgbz[p++];
        palsize = (fgbz[p] << 8) | fgbz[p + 1]; p += 2;
        if ((size_t)p + (size_t)palsize * 3 <= sz) {
            pal = (uint8_t *)djvu_alloc(ctx, (size_t)palsize * 3);
            if (pal) memcpy(pal, fgbz + p, (size_t)palsize * 3);
            p += (size_t)palsize * 3;
            if ((version & 0x80) && p + 3 <= sz) {
                int datasize = (fgbz[p] << 16) | (fgbz[p+1] << 8) | fgbz[p+2]; p += 3;
                size_t dlen = 0;
                uint8_t *dd = djvu_bzz_decode_all(ctx, fgbz + p, sz - p, &dlen);
                if (dd && (size_t)datasize * 2 <= dlen) {
                    colordata = (short *)djvu_alloc(ctx, sizeof(short) * datasize);
                    if (colordata) {
                        for (i = 0; i < datasize; i++)
                            colordata[i] = (short)((dd[i*2] << 8) | dd[i*2+1]);
                        ncolor = datasize;
                    }
                }
                djvu_free(ctx, dd);
            }
        }
    }

    if (!pal) {
        double tfg = 0.0;
        if (t) tfg = djvu_bench_now_ms();
        fgpm = djvu_doc_iw44_acquire(doc, page_no, "FG44", &fg_owned);
        if (fgpm) {
            int fw = djvu_iw44_width(fgpm);
            int fh = djvu_iw44_height(fgpm);
            fgred = djvu_compute_red(width, height, fw, fh);
            if (fgred < 1) fgred = 1;
            if (djvu_cpix_init(ctx, &fgnat, fw, fh) != 0 ||
                djvu_iw44_render_rgb_raw(fgpm, fgnat.d) != 0)
                fgpm = NULL;
        }
        if (t) t->iw44_ms += djvu_bench_now_ms() - tfg;
    }

    for (i = 0; mask && i < mask->nblits; i++) {
        jb2_blit *b = &mask->blits[i];
        jb2_shape *s = djvu_jb2_get_shape(mask, b->shapeno);
        compose_ink_ctx ink;
        if (!s || !djvu_bm_has_pixels(&s->bm)) continue;
        ink.bg = &bg;
        ink.palr = ink.palg = ink.palb = 0;
        ink.has_pal = ink.has_fg = 0;
        ink.fgred = fgred;
        ink.fgnat = &fgnat;
        if (pal && colordata && i < ncolor) {
            int ci = colordata[i];
            if (ci >= 0 && ci < palsize) {
                ink.palb = pal[ci*3+0]; ink.palg = pal[ci*3+1]; ink.palr = pal[ci*3+2];
                ink.has_pal = 1;
            }
        } else if (fgpm) {
            ink.has_fg = 1;
        }
        djvu_bm_visit_ink(&s->bm, b->left, b->bottom, compose_stamp_ink, &ink);
    }

    if (t) t->composite_ms += djvu_bench_now_ms() - t0;

    djvu_free(ctx, pal); djvu_free(ctx, colordata);
    djvu_cpix_free(ctx, &fgnat);
    djvu_doc_iw44_release(ctx, fgpm, fg_owned);
    *bgout = bg;
    return 0;
}

static int compose_gamma_lut(djvu_doc *doc, uint32_t form_off, unsigned char *lut)
{
    return build_gamma_lut(2.2 / page_gamma(doc, form_off), lut);
}

static int compose_page_fgbz_direct_rgb(djvu_doc *doc, int page_no,
                                        jb2_image *mask,
                                        int width, int height,
                                        uint8_t *dst, int stride,
                                        djvu_render_timings *t)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off = doc->pages[page_no].form_off;
    uint32_t sz;
    const uint8_t *fgbz;
    unsigned char lut[256];
    fgbz_palette fg;
    double t0 = 0.0;
    int rc = -1;

    memset(&fg, 0, sizeof(fg));
    if (!mask || ctx->bgr) return -1;
    if (compose_gamma_lut(doc, form_off, lut)) return -1;
    if (djvu_form_find_chunk(doc, form_off, "FG44", &sz, NULL) != NULL)
        return -1;
    fgbz = djvu_form_find_chunk(doc, form_off, "FGbz", &sz, NULL);
    if (fgbz_palette_parse(ctx, fgbz, sz, &fg) != 0)
        return -1;

    if (compose_background_topdown_rgb(doc, form_off, width, height,
                                       dst, stride, t) != 0)
        goto done;

    if (t) t0 = djvu_bench_now_ms();
    rc = compose_fgbz_stencil_topdown_rgb(mask, &fg, width, height, dst, stride);
    if (t) t->composite_ms += djvu_bench_now_ms() - t0;

done:
    fgbz_palette_free(ctx, &fg);
    return rc;
}

djvu_image *djvu_compose_page(djvu_doc *doc, int page_no, jb2_image *mask,
                             int width, int height, djvu_render_timings *t)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off = doc->pages[page_no].form_off;
    djvu_cpix bg; djvu_image *out;
    unsigned char lut[256]; const unsigned char *lp = NULL;

    memset(&bg, 0, sizeof(bg));
    if (mask && !ctx->bgr &&
        djvu_form_find_chunk(doc, form_off, "FGbz", NULL, NULL) != NULL &&
        djvu_form_find_chunk(doc, form_off, "FG44", NULL, NULL) == NULL &&
        !compose_gamma_lut(doc, form_off, lut)) {
        out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
        if (out) {
            out->width = width;
            out->height = height;
            out->format = DJVU_FORMAT_RGB24;
            out->stride = width * 3;
            out->data = (uint8_t *)djvu_alloc(ctx, (size_t)width * height * 3);
            if (!out->data) {
                djvu_free(ctx, out);
                out = NULL;
            } else if (compose_page_fgbz_direct_rgb(doc, page_no, mask, width, height,
                                                    out->data, out->stride, t) == 0) {
                return out;
            } else {
                djvu_image_destroy(ctx, out);
                out = NULL;
            }
        }
    }

    if (compose_to_bg(doc, page_no, mask, width, height, t, &bg) != 0)
        return NULL;
    if (compose_gamma_lut(doc, form_off, lut)) lp = lut;

    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (out) {
        out->width = bg.w; out->height = bg.h; out->format = DJVU_FORMAT_RGB24;
        out->stride = bg.w * 3;
        out->data = (uint8_t *)djvu_alloc(ctx, (size_t)bg.w * bg.h * 3);
        if (out->data)
            compose_finalize(out->data, bg.w * 3, &bg, ctx->bgr, lp);
        else { djvu_free(ctx, out); out = NULL; }
    }
    djvu_cpix_free(ctx, &bg);
    return out;
}

int djvu_compose_page_into(djvu_doc *doc, int page_no, jb2_image *mask,
                           int width, int height, uint8_t *dst, int stride)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off = doc->pages[page_no].form_off;
    djvu_cpix bg;
    unsigned char lut[256]; const unsigned char *lp = NULL;

    memset(&bg, 0, sizeof(bg));
    if (compose_page_fgbz_direct_rgb(doc, page_no, mask, width, height,
                                     dst, stride, NULL) == 0)
        return 0;
    if (compose_to_bg(doc, page_no, mask, width, height, NULL, &bg) != 0)
        return -1;
    if (compose_gamma_lut(doc, form_off, lut)) lp = lut;
    compose_finalize(dst, stride, &bg, ctx->bgr, lp);
    djvu_cpix_free(ctx, &bg);
    return 0;
}
