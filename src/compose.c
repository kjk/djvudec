/* compose.c -- page compositing: background (IW44, upsampled) + foreground
 * (FG44 or FGbz palette) stenciled through the JB2 mask.
 * Scaler in scaler.c; stencil ported from DjVuLibre GPixmap.cpp / DjVuImage.cpp.
 * Pixmaps are RGB bottom-up; output is flipped to top-down at the end. */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static iw_pixmap *decode_bg(djvu_doc *doc, uint32_t form_off)
{
    iw_pixmap *pm = djvu_iw44_new(doc->ctx);
    if (!pm) return NULL;
    if (djvu_iw44_decode_form(doc, form_off, "BG44", pm, 0) != 0) {
        djvu_iw44_free(pm);
        return NULL;
    }
    return pm;
}

int djvu_compose_background(djvu_doc *doc, uint32_t form_off, int width, int height,
                            djvu_cpix *out)
{
    djvu_ctx *ctx = doc->ctx;
    iw_pixmap *pm = decode_bg(doc, form_off);
    int bw, bh, red, rc = -1;
    djvu_cpix native;
    memset(&native, 0, sizeof(native));
    if (!pm) return -1;
    bw = djvu_iw44_width(pm); bh = djvu_iw44_height(pm);
    red = djvu_compute_red(width, height, bw, bh);
    if (red < 1) { djvu_iw44_free(pm); return -1; }
    if (djvu_cpix_init(ctx, &native, bw, bh) != 0) { djvu_iw44_free(pm); return -1; }
    if (djvu_iw44_render_rgb_raw(pm, native.d) != 0) goto done;
    if (red == 1) {
        *out = native; native.d = NULL; rc = 0;
    } else {
        rc = djvu_cpix_scale(ctx, &native, out, width, height, red);
    }
done:
    djvu_cpix_free(ctx, &native);
    djvu_iw44_free(pm);
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

djvu_image *djvu_compose_page(djvu_doc *doc, int page_no, jb2_image *mask,
                             int width, int height, djvu_render_timings *t)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off = doc->pages[page_no].form_off;
    djvu_cpix bg; djvu_image *out = NULL;
    uint32_t sz; const uint8_t *fgbz, *fg44chunk;
    uint8_t *pal = NULL; int palsize = 0;
    short *colordata = NULL; int ncolor = 0;
    iw_pixmap *fgpm = NULL; djvu_cpix fgnat; int fgred = 0;
    int i;
    double t0;

    memset(&bg, 0, sizeof(bg)); memset(&fgnat, 0, sizeof(fgnat));
    if (t) t0 = djvu_bench_now_ms();
    if (djvu_compose_background(doc, form_off, width, height, &bg) != 0)
        return NULL;
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
        double tfg;
        if (t) tfg = djvu_bench_now_ms();
        fg44chunk = djvu_form_find_chunk(doc, form_off, "FG44", &sz, NULL);
        if (fg44chunk) {
            int fw, fh;
            fgpm = djvu_iw44_new(ctx);
            if (fgpm && djvu_iw44_decode_form(doc, form_off, "FG44", fgpm, 0) == 0) {
                fw = djvu_iw44_width(fgpm);
                fh = djvu_iw44_height(fgpm);
                fgred = djvu_compute_red(width, height, fw, fh);
                if (fgred < 1) fgred = 1;
                if (djvu_cpix_init(ctx, &fgnat, fw, fh) != 0 ||
                    djvu_iw44_render_rgb_raw(fgpm, fgnat.d) != 0) {
                    djvu_iw44_free(fgpm); fgpm = NULL;
                }
            } else {
                djvu_iw44_free(fgpm); fgpm = NULL;
            }
        }
        if (t) t->iw44_ms += djvu_bench_now_ms() - tfg;
    }

    for (i = 0; mask && i < mask->nblits; i++) {
        jb2_blit *b = &mask->blits[i];
        jb2_shape *s = djvu_jb2_get_shape(mask, b->shapeno);
        int sw, sh, rr, cc, palr = 0, palg = 0, palb = 0;
        if (!s || !s->bm.data) continue;
        sw = s->bm.width; sh = s->bm.height;
        if (pal && colordata && i < ncolor) {
            int ci = colordata[i];
            if (ci >= 0 && ci < palsize) {
                palb = pal[ci*3+0]; palg = pal[ci*3+1]; palr = pal[ci*3+2];
            }
        }
        for (rr = 0; rr < sh; rr++) {
            int srow = djvu_bm_rowoffset(&s->bm, rr);
            int py = b->bottom + rr;
            if (py < 0 || py >= bg.h) continue;
            for (cc = 0; cc < sw; cc++) {
                int px = b->left + cc;
                uint8_t *d;
                if (px < 0 || px >= bg.w) continue;
                if (s->bm.data[srow + cc] == 0) continue;
                d = bg.d + ((size_t)py * bg.w + px) * 3;
                if (pal) {
                    d[0] = (uint8_t)palr; d[1] = (uint8_t)palg; d[2] = (uint8_t)palb;
                } else if (fgpm) {
                    int fx = px / fgred, fy = py / fgred;
                    if (fx >= fgnat.w) fx = fgnat.w - 1;
                    if (fy >= fgnat.h) fy = fgnat.h - 1;
                    {
                        uint8_t *f = fgnat.d + ((size_t)fy * fgnat.w + fx) * 3;
                        d[0] = f[0]; d[1] = f[1]; d[2] = f[2];
                    }
                } else {
                    d[0] = d[1] = d[2] = 0;
                }
            }
        }
    }

    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (out) {
        out->width = bg.w; out->height = bg.h; out->format = DJVU_FORMAT_RGB24;
        out->stride = bg.w * 3;
        out->data = (uint8_t *)djvu_alloc(ctx, (size_t)bg.w * bg.h * 3);
        if (out->data) {
            djvu_flip_rgb_bottomup(out->data, bg.d, bg.w, bg.h);
            {
                unsigned char lut[256];
                if (build_gamma_lut(2.2 / page_gamma(doc, form_off), lut)) {
                    size_t k, npx = (size_t)bg.w * bg.h * 3;
                    for (k = 0; k < npx; k++) out->data[k] = lut[out->data[k]];
                }
            }
        } else { djvu_free(ctx, out); out = NULL; }
    }

    if (t) t->composite_ms += djvu_bench_now_ms() - t0;

    djvu_free(ctx, pal); djvu_free(ctx, colordata);
    djvu_cpix_free(ctx, &fgnat); djvu_iw44_free(fgpm);
    djvu_cpix_free(ctx, &bg);
    return out;
}