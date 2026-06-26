/* scaler.c -- GPixmapScaler bilinear upsampler (ported from DjVuLibre).
 * Used by compose.c to scale IW44 background/foreground to page resolution. */
#include "djvu_internal.h"
#include <string.h>

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

void djvu_init(void)
{
    prepare_interp();
}

void djvu_scaler_init(void)
{
    djvu_init();
}

static int imini(int a, int b) { return a < b ? a : b; }

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

static void scaler_get_line(scaler *s, int fy, const djvu_cpix *in, int in_x0, int in_y0,
                            int red_xmin, int red_xmax, uint8_t *out)
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

static int scaler_scale(scaler *s, const djvu_cpix *in, djvu_cpix *out)
{
    djvu_ctx *ctx = s->ctx;
    int bufw, y;
    uint8_t *lbuf;
    uint8_t *p1 = NULL, *p2 = NULL; int l1 = -1, l2 = -1;
    int red_xmin = 0, red_xmax = s->redw;

    prepare_interp();
    if (!s->hcoord) scaler_set_h(s, 0, 0);
    if (!s->vcoord) scaler_set_v(s, 0, 0);
    if (djvu_cpix_init(ctx, out, s->outw, s->outh) != 0) return -1;
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
        deltas = &s_interp[fy & FRACMASK][256];
        for (x = 0; x < bufw; x++) {
            int lr = lower[x*3+0], lg = lower[x*3+1], lb = lower[x*3+2];
            lbuf[(x+1)*3+0] = (uint8_t)(lr + deltas[upper[x*3+0] - lr]);
            lbuf[(x+1)*3+1] = (uint8_t)(lg + deltas[upper[x*3+1] - lg]);
            lbuf[(x+1)*3+2] = (uint8_t)(lb + deltas[upper[x*3+2] - lb]);
        }
        lbuf[0]=lbuf[3]; lbuf[1]=lbuf[4]; lbuf[2]=lbuf[5];
        lbuf[(bufw+1)*3+0]=lbuf[bufw*3+0]; lbuf[(bufw+1)*3+1]=lbuf[bufw*3+1]; lbuf[(bufw+1)*3+2]=lbuf[bufw*3+2];
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

int djvu_cpix_init(djvu_ctx *ctx, djvu_cpix *p, int w, int h)
{
    djvu_free(ctx, p->d);
    p->w = w; p->h = h;
    p->d = (uint8_t *)djvu_alloc(ctx, (size_t)w * h * 3);
    if (!p->d) return -1;
    memset(p->d, 0, (size_t)w * h * 3);
    return 0;
}

void djvu_cpix_free(djvu_ctx *ctx, djvu_cpix *p)
{
    if (p) { djvu_free(ctx, p->d); p->d = NULL; }
}

int djvu_compute_red(int w, int h, int rw, int rh)
{
    int red;
    for (red = 1; red < 16; red++)
        if (((w + red - 1) / red == rw) && ((h + red - 1) / red == rh))
            return red;
    return 0;
}

int djvu_cpix_scale(djvu_ctx *ctx, const djvu_cpix *in, djvu_cpix *out,
                    int outw, int outh, int red)
{
    scaler s;
    memset(&s, 0, sizeof(s));
    s.ctx = ctx;
    s.inw = in->w;
    s.inh = in->h;
    s.outw = outw;
    s.outh = outh;
    scaler_set_h(&s, red, 1);
    scaler_set_v(&s, red, 1);
    if (scaler_scale(&s, in, out) != 0) { scaler_free(&s); return -1; }
    scaler_free(&s);
    return 0;
}

void djvu_flip_rgb_bottomup(uint8_t *dst, const uint8_t *src, int w, int h, int bgr)
{
    int y, x;
    size_t row = (size_t)w * 3;
    if (!bgr) {
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * row, src + (size_t)(h - 1 - y) * row, row);
        return;
    }
    /* flip and swap R<->B in one pass (B,G,R output) */
    for (y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * row;
        const uint8_t *s = src + (size_t)(h - 1 - y) * row;
        for (x = 0; x < w; x++) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d += 3;
            s += 3;
        }
    }
}