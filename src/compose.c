/* compose.c -- page compositing: background (IW44, upsampled) + foreground
 * (FG44 or FGbz palette) stenciled through the JB2 mask.
 * Scaler + stencil ported from DjVuLibre GScaler.cpp / GPixmap.cpp /
 * DjVuImage.cpp. Pixmaps here are RGB (3 bytes), bottom-up (DjVu convention);
 * the page is flipped to top-down at the very end. */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------- simple RGB pixmap ---------- */
typedef struct { int w, h; uint8_t *d; } cpix;  /* d = w*h*3, R,G,B */

static int cpix_init(djvu_ctx *ctx, cpix *p, int w, int h)
{
    djvu_free(ctx, p->d);
    p->w = w; p->h = h;
    p->d = (uint8_t *)djvu_alloc(ctx, (size_t)w * h * 3);
    if (!p->d) return -1;
    memset(p->d, 0, (size_t)w * h * 3);
    return 0;
}
static void cpix_free(djvu_ctx *ctx, cpix *p) { if (p) { djvu_free(ctx, p->d); p->d = NULL; } }

/* ---------- scaler (GPixmapScaler) ---------- */

#define FRACBITS 4
#define FRACSIZE (1 << FRACBITS)
#define FRACSIZE2 (FRACSIZE >> 1)
#define FRACMASK (FRACSIZE - 1)

static short s_interp[FRACSIZE][512];
static int s_interp_ready = 0;

static void prepare_interp(void)
{
    int i, j;
    if (s_interp_ready) return;
    for (i = 0; i < FRACSIZE; i++) {
        short *d = &s_interp[i][256];
        for (j = -256; j < 256; j++)
            d[j] = (short)((j * i + FRACSIZE2) >> FRACBITS);
    }
    s_interp_ready = 1;
}

static int imini(int a, int b) { return a < b ? a : b; }
static int imaxi(int a, int b) { return a > b ? a : b; }

static void prepare_coord(int *coord, int inmax, int outmax, int in, int out)
{
    int len = in * FRACSIZE;
    int beg = (len + out) / (2 * out) - FRACSIZE2;
    int y = beg, z = out / 2;
    int inmaxlim = (inmax - 1) * FRACSIZE;
    int x;
    for (x = 0; x < outmax; x++) {
        coord[x] = imini(y, inmaxlim);
        z = z + len;
        y = y + z / out;
        z = z % out;
    }
}

typedef struct {
    djvu_ctx *ctx;
    int inw, inh, outw, outh;
    int xshift, yshift, redw, redh;
    int *hcoord, *vcoord;
} scaler;

static void scaler_set_h(scaler *s, int numer, int denom)
{
    if (numer == 0 && denom == 0) { numer = s->outw; denom = s->inw; }
    s->xshift = 0; s->redw = s->inw;
    while (numer + numer < denom) { s->xshift++; s->redw = (s->redw + 1) >> 1; numer <<= 1; }
    s->hcoord = (int *)djvu_alloc(s->ctx, sizeof(int) * s->outw);
    prepare_coord(s->hcoord, s->redw, s->outw, denom, numer);
}
static void scaler_set_v(scaler *s, int numer, int denom)
{
    if (numer == 0 && denom == 0) { numer = s->outh; denom = s->inh; }
    s->yshift = 0; s->redh = s->inh;
    while (numer + numer < denom) { s->yshift++; s->redh = (s->redh + 1) >> 1; numer <<= 1; }
    s->vcoord = (int *)djvu_alloc(s->ctx, sizeof(int) * s->outh);
    prepare_coord(s->vcoord, s->redh, s->outh, denom, numer);
}

/* reduce one input row-block to a reduced line (box average), like get_line. */
static void scaler_get_line(scaler *s, int fy, const cpix *in, int in_x0, int in_y0,
                            int red_xmin, int red_xmax, uint8_t *out /*redw*3 of segment*/)
{
    int sw = 1 << s->xshift;
    int div = s->xshift + s->yshift;
    int rnd = div ? (1 << (div - 1)) : 0;
    int x, idx = 0;
    int ly0 = (fy << s->yshift);
    int ly1 = ((fy + 1) << s->yshift);
    int xmin = red_xmin << s->xshift, xmax = red_xmax << s->xshift;
    if (ly1 > in->h + in_y0) ly1 = in->h + in_y0;
    for (x = xmin; x < xmax; x += sw) {
        int r = 0, g = 0, b = 0, ss = 0, sy, sx;
        for (sy = ly0; sy < ly1; sy++) {
            int rowy = sy - in_y0;
            for (sx = x; sx < x + sw && sx < xmax; sx++) {
                int px = sx - in_x0;
                const uint8_t *p = in->d + ((size_t)rowy * in->w + px) * 3;
                r += p[0]; g += p[1]; b += p[2]; ss++;
            }
        }
        if (ss == rnd + rnd && div) {
            out[idx*3+0]=(uint8_t)((r+rnd)>>div); out[idx*3+1]=(uint8_t)((g+rnd)>>div); out[idx*3+2]=(uint8_t)((b+rnd)>>div);
        } else if (ss) {
            out[idx*3+0]=(uint8_t)((r+ss/2)/ss); out[idx*3+1]=(uint8_t)((g+ss/2)/ss); out[idx*3+2]=(uint8_t)((b+ss/2)/ss);
        }
        idx++;
    }
}

/* Scale `in` (full image, top-left at provided origin 0,0) to `out` (outw x outh).
   Only the common case provided_input == whole input is supported. */
static int scaler_scale(scaler *s, const cpix *in, cpix *out)
{
    djvu_ctx *ctx = s->ctx;
    int bufw, y;
    uint8_t *lbuf;       /* (bufw+2)*3 */
    uint8_t *p1 = NULL, *p2 = NULL; int l1 = -1, l2 = -1;
    int red_xmin = 0, red_xmax = s->redw;

    prepare_interp();
    if (!s->hcoord) scaler_set_h(s, 0, 0);
    if (!s->vcoord) scaler_set_v(s, 0, 0);
    if (cpix_init(ctx, out, s->outw, s->outh) != 0) return -1;
    bufw = s->redw;
    lbuf = (uint8_t *)djvu_alloc(ctx, (size_t)(bufw + 2) * 3);
    if (!lbuf) return -1;
    if (s->xshift > 0 || s->yshift > 0) {
        p1 = (uint8_t *)djvu_alloc(ctx, (size_t)bufw * 3);
        p2 = (uint8_t *)djvu_alloc(ctx, (size_t)bufw * 3);
        if (!p1 || !p2) { djvu_free(ctx, lbuf); djvu_free(ctx, p1); djvu_free(ctx, p2); return -1; }
    }

    for (y = 0; y < s->outh; y++) {
        int fy = s->vcoord[y];
        int fy1 = fy >> FRACBITS, fy2 = fy1 + 1;
        const uint8_t *lower, *upper;
        const short *deltas;
        uint8_t *dest;
        int x;

        if (s->xshift > 0 || s->yshift > 0) {
            /* reduced-line cache */
            int want1 = fy1 < 0 ? 0 : (fy1 >= s->redh ? s->redh - 1 : fy1);
            int want2 = fy2 < 0 ? 0 : (fy2 >= s->redh ? s->redh - 1 : fy2);
            if (want1 == l2) lower = p2; else if (want1 == l1) lower = p1;
            else { uint8_t *t = p1; p1 = p2; l1 = l2; p2 = t; l2 = want1;
                   scaler_get_line(s, want1, in, 0, 0, red_xmin, red_xmax, p2); lower = p2; }
            if (want2 == l2) upper = p2; else if (want2 == l1) upper = p1;
            else { uint8_t *t = p1; p1 = p2; l1 = l2; p2 = t; l2 = want2;
                   scaler_get_line(s, want2, in, 0, 0, red_xmin, red_xmax, p2); upper = p2; }
        } else {
            if (fy1 < 0) fy1 = 0; if (fy1 > s->redh - 1) fy1 = s->redh - 1;
            if (fy2 < 0) fy2 = 0; if (fy2 > s->redh - 1) fy2 = s->redh - 1;
            lower = in->d + (size_t)fy1 * in->w * 3;
            upper = in->d + (size_t)fy2 * in->w * 3;
        }
        /* vertical interp into lbuf[1..bufw] */
        deltas = &s_interp[fy & FRACMASK][256];
        for (x = 0; x < bufw; x++) {
            int lr = lower[x*3+0], lg = lower[x*3+1], lb = lower[x*3+2];
            lbuf[(x+1)*3+0] = (uint8_t)(lr + deltas[upper[x*3+0] - lr]);
            lbuf[(x+1)*3+1] = (uint8_t)(lg + deltas[upper[x*3+1] - lg]);
            lbuf[(x+1)*3+2] = (uint8_t)(lb + deltas[upper[x*3+2] - lb]);
        }
        lbuf[0]=lbuf[3]; lbuf[1]=lbuf[4]; lbuf[2]=lbuf[5];
        lbuf[(bufw+1)*3+0]=lbuf[bufw*3+0]; lbuf[(bufw+1)*3+1]=lbuf[bufw*3+1]; lbuf[(bufw+1)*3+2]=lbuf[bufw*3+2];
        /* horizontal interp */
        dest = out->d + (size_t)y * s->outw * 3;
        for (x = 0; x < s->outw; x++) {
            int n = s->hcoord[x];
            const uint8_t *lo = lbuf + (1 + (n >> FRACBITS) - red_xmin) * 3;
            const short *dh = &s_interp[n & FRACMASK][256];
            int lr = lo[0], lg = lo[1], lb = lo[2];
            dest[x*3+0] = (uint8_t)(lr + dh[lo[3] - lr]);
            dest[x*3+1] = (uint8_t)(lg + dh[lo[4] - lg]);
            dest[x*3+2] = (uint8_t)(lb + dh[lo[5] - lb]);
        }
    }
    djvu_free(ctx, lbuf); djvu_free(ctx, p1); djvu_free(ctx, p2);
    return 0;
}

static void scaler_free(scaler *s)
{
    djvu_free(s->ctx, s->hcoord);
    djvu_free(s->ctx, s->vcoord);
}

/* compute_red: DjVuLibre's reduction-factor heuristic */
static int compute_red(int w, int h, int rw, int rh)
{
    int red;
    for (red = 1; red < 16; red++)
        if (((w + red - 1) / red == rw) && ((h + red - 1) / red == rh))
            return red;
    return 0;
}

/* Decode the page background (BG44) into `bg` (bottom-up RGB, native res). */
static iw_pixmap *decode_bg(djvu_doc *doc, uint32_t form_off)
{
    uint32_t start = 0, sz; const uint8_t *chunk;
    iw_pixmap *pm = djvu_iw44_new(doc->ctx);
    int n = 0;
    if (!pm) return NULL;
    while ((chunk = djvu_form_find_chunk(doc, form_off, "BG44", &sz, &start)) != NULL) {
        if (djvu_iw44_decode_chunk(pm, chunk, sz) != 0) { djvu_iw44_free(pm); return NULL; }
        n++;
    }
    if (!n) { djvu_iw44_free(pm); return NULL; }
    return pm;
}

/* Produce the full-resolution background pixmap (bottom-up RGB). Returns 0 ok. */
int djvu_compose_background(djvu_doc *doc, uint32_t form_off, int width, int height, cpix *out);
int djvu_compose_background(djvu_doc *doc, uint32_t form_off, int width, int height, cpix *out)
{
    djvu_ctx *ctx = doc->ctx;
    iw_pixmap *pm = decode_bg(doc, form_off);
    int bw, bh, red, rc = -1;
    cpix native;
    memset(&native, 0, sizeof(native));
    if (!pm) return -1;
    bw = djvu_iw44_width(pm); bh = djvu_iw44_height(pm);
    red = compute_red(width, height, bw, bh);
    if (red < 1) { djvu_iw44_free(pm); return -1; }
    if (cpix_init(ctx, &native, bw, bh) != 0) { djvu_iw44_free(pm); return -1; }
    if (djvu_iw44_render_rgb_raw(pm, native.d) != 0) goto done;
    if (red == 1) {
        *out = native; native.d = NULL; rc = 0;  /* move */
    } else {
        scaler s; memset(&s, 0, sizeof(s));
        s.ctx = ctx; s.inw = bw; s.inh = bh; s.outw = width; s.outh = height;
        scaler_set_h(&s, red, 1);
        scaler_set_v(&s, red, 1);
        rc = scaler_scale(&s, &native, out);
        scaler_free(&s);
    }
done:
    cpix_free(ctx, &native);
    djvu_iw44_free(pm);
    return rc;
}

/* Build DjVuLibre's gamma-correction LUT for correction factor `corr`
   (corr = target_gamma / document_gamma). lut[i] = round(255*(i/255)^(1/corr)),
   with endpoints forced to 0/255 (white = WHITE). Returns 1 if non-trivial. */
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

/* Read the page's document gamma from its INFO chunk (default 2.2). */
static double page_gamma(djvu_doc *doc, uint32_t form_off)
{
    uint32_t sz;
    const uint8_t *info = djvu_form_find_chunk(doc, form_off, "INFO", &sz, NULL);
    if (info && sz >= 9 && info[8] != 0)
        return (double)info[8] / 10.0;
    return 2.2;
}

/* Composite mask + background + foreground into a top-down RGB djvu_image.
   `mask` is the decoded Sjbz JB2 image. Returns NULL on error. */
djvu_image *djvu_compose_page(djvu_doc *doc, int page_no, jb2_image *mask,
                             int width, int height)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off = doc->pages[page_no].form_off;
    cpix bg; djvu_image *out = NULL;
    uint32_t sz; const uint8_t *fgbz, *fg44chunk;
    /* foreground sources */
    uint8_t *pal = NULL; int palsize = 0;     /* FGbz: palsize*3 bytes (b,g,r) */
    short *colordata = NULL; int ncolor = 0;  /* FGbz: per-blit palette index */
    iw_pixmap *fgpm = NULL; cpix fgnat; int fgred = 0;  /* FG44 */
    int i, y, x;

    memset(&bg, 0, sizeof(bg)); memset(&fgnat, 0, sizeof(fgnat));
    if (djvu_compose_background(doc, form_off, width, height, &bg) != 0)
        return NULL;

    /* ----- foreground: FGbz palette (two-layer) ----- */
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

    /* ----- foreground: FG44 pixmap (three-layer) ----- */
    if (!pal) {
        uint32_t start = 0; const uint8_t *fc; int n = 0;
        fg44chunk = djvu_form_find_chunk(doc, form_off, "FG44", &sz, &start);
        if (fg44chunk) {
            fgpm = djvu_iw44_new(ctx);
            start = 0;
            while ((fc = djvu_form_find_chunk(doc, form_off, "FG44", &sz, &start)) != NULL) {
                if (djvu_iw44_decode_chunk(fgpm, fc, sz) != 0) { djvu_iw44_free(fgpm); fgpm = NULL; break; }
                n++;
            }
            if (fgpm && n) {
                int fw = djvu_iw44_width(fgpm), fh = djvu_iw44_height(fgpm);
                fgred = compute_red(width, height, fw, fh);
                if (fgred < 1) fgred = 1;
                if (cpix_init(ctx, &fgnat, fw, fh) != 0 ||
                    djvu_iw44_render_rgb_raw(fgpm, fgnat.d) != 0) {
                    djvu_iw44_free(fgpm); fgpm = NULL;
                }
            }
        }
    }

    /* ----- composite (bottom-up): for each ink pixel, write the fg color ----- */
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
                if (s->bm.data[srow + cc] == 0) continue;   /* not ink */
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
                    d[0] = d[1] = d[2] = 0;   /* no fg color -> black ink */
                }
            }
        }
    }

    /* emit top-down RGB */
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (out) {
        out->width = bg.w; out->height = bg.h; out->format = DJVU_FORMAT_RGB24;
        out->stride = bg.w * 3;
        out->data = (uint8_t *)djvu_alloc(ctx, (size_t)bg.w * bg.h * 3);
        if (out->data) {
            for (y = 0; y < bg.h; y++)
                memcpy(out->data + (size_t)y * bg.w * 3,
                       bg.d + (size_t)(bg.h - 1 - y) * bg.w * 3, (size_t)bg.w * 3);
            /* gamma correction (target 2.2 / document gamma), as ddjvu does */
            {
                unsigned char lut[256];
                if (build_gamma_lut(2.2 / page_gamma(doc, form_off), lut)) {
                    size_t k, npx = (size_t)bg.w * bg.h * 3;
                    for (k = 0; k < npx; k++) out->data[k] = lut[out->data[k]];
                }
            }
        } else { djvu_free(ctx, out); out = NULL; }
    }
    (void)x;

    djvu_free(ctx, pal); djvu_free(ctx, colordata);
    cpix_free(ctx, &fgnat); djvu_iw44_free(fgpm);
    cpix_free(ctx, &bg);
    return out;
}

/* debug: full-page background as a top-down djvu_image (for ddjvu -mode=background) */
djvu_image *djvu_debug_render_bg(djvu_doc *doc, int page_no)
{
    djvu_ctx *ctx; djvu_page_info info; cpix bg; djvu_image *out; int y;
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    if (djvu_doc_page_info(doc, page_no, &info) != 0) return NULL;
    memset(&bg, 0, sizeof(bg));
    if (djvu_compose_background(doc, doc->pages[page_no].form_off,
                               info.width, info.height, &bg) != 0) return NULL;
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { cpix_free(ctx, &bg); return NULL; }
    out->width = bg.w; out->height = bg.h; out->format = DJVU_FORMAT_RGB24;
    out->stride = bg.w * 3;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)bg.w * bg.h * 3);
    if (!out->data) { djvu_free(ctx, out); cpix_free(ctx, &bg); return NULL; }
    for (y = 0; y < bg.h; y++)  /* bottom-up -> top-down */
        memcpy(out->data + (size_t)y * bg.w * 3,
               bg.d + (size_t)(bg.h - 1 - y) * bg.w * 3, (size_t)bg.w * 3);
    cpix_free(ctx, &bg);
    return out;
}
