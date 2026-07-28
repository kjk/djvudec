/* scaler.c -- GPixmapScaler bilinear upsampler (ported from DjVuLibre).
 * Used by compose.c to scale IW44 background/foreground to page resolution. */
#include "djvu_internal.h"
#include <string.h>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define DJVU_SCALER_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#define DJVU_SCALER_NEON 1
#include <arm_neon.h>
#endif

#define FRACBITS 4
#define FRACSIZE (1 << FRACBITS)
#define FRACSIZE2 (FRACSIZE >> 1)
#define FRACMASK (FRACSIZE - 1)

static short s_interp[FRACSIZE][512];
static uint32_t s_interp3_delta[512];
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
    for (j = -255; j <= 255; j++) {
        uint16_t d6 = (uint16_t)s_interp[6][256 + j];
        uint16_t d11 = (uint16_t)s_interp[11][256 + j];
        s_interp3_delta[255 + j] = (uint32_t)d6 | ((uint32_t)d11 << 16);
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
    int hnum, hden, vnum, vden;   /* scale ratio as passed to set_h/set_v */
    int *hcoord, *vcoord;
} scaler;

static void scaler_set_h(scaler *s, int numer, int denom)
{
    if (numer == 0 && denom == 0) { numer = s->outw; denom = s->inw; }
    s->hnum = numer; s->hden = denom;
    s->xshift = 0; s->redw = s->inw;
    while (numer + numer < denom) { s->xshift++; s->redw = (s->redw + 1) >> 1; numer <<= 1; }
    s->hcoord = (int *)djvu_alloc(s->ctx, sizeof(int) * s->outw);
    prepare_coord(s->hcoord, s->redw, s->outw, denom, numer);
}
static void scaler_set_v(scaler *s, int numer, int denom)
{
    if (numer == 0 && denom == 0) { numer = s->outh; denom = s->inh; }
    s->vnum = numer; s->vden = denom;
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
    int xend = in_x0 + in->w, yend = in_y0 + in->h;
    if (xmax > xend) xmax = xend;
    if (ly0 < in_y0) ly0 = in_y0;
    if (ly1 > yend) ly1 = yend;
    for (x = xmin; x < xmax; x += sw) {
        int r = 0, g = 0, b = 0, ss = 0, sy, sx;
        for (sy = ly0; sy < ly1; sy++) {
            int rowy = sy - in_y0;
            if (rowy < 0 || rowy >= in->h)
                continue;
            for (sx = x; sx < x + sw && sx < xmax; sx++) {
                int px = sx - in_x0;
                if (px < 0 || px >= in->w)
                    continue;
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
    {
        int nout = red_xmax - red_xmin;
        while (idx < nout) {
            if (idx > 0) {
                out[idx * 3 + 0] = out[(idx - 1) * 3 + 0];
                out[idx * 3 + 1] = out[(idx - 1) * 3 + 1];
                out[idx * 3 + 2] = out[(idx - 1) * 3 + 2];
            } else {
                out[0] = out[1] = out[2] = 0;
            }
            idx++;
        }
    }
}

static int cpix_init_uninit(djvu_ctx *ctx, djvu_cpix *p, int w, int h)
{
    djvu_free(ctx, p->d);
    p->w = w; p->h = h;
    p->d = (uint8_t *)djvu_alloc(ctx, (size_t)w * h * 3);
    return p->d ? 0 : -1;
}

static uint8_t *scaler_dest_row(uint8_t *dst0, int stride, int outh,
                                int topdown, int y)
{
    return dst0 + (size_t)(topdown ? (outh - 1 - y) : y) * stride;
}

/* Expand one row w -> outw for the ceil-3x upscale (outw in [3w-2, 3w]).
   Emits the exact prepare_coord(red=3) sample sequence: src[0] once, then
   per input pixel the (0, 6/16, 11/16) interpolation triple, then the last
   pixel replicated outw-(3w-2) more times (the trailing clamped samples). */
static void scaler_expand_row3(const uint8_t *src, int w, int outw, uint8_t *dst)
{
    const uint8_t *last;
    uint8_t *d = dst;
    int x, ntail = outw - (3 * w - 2);

    d[0] = src[0];
    d[1] = src[1];
    d[2] = src[2];
    d += 3;

    x = 0;
#ifdef DJVU_SCALER_NEON
    /* 8 source pairs: vld3 loads planar R/G/B for a[x..x+7] and b[x+1..x+8].
       mid/far use the same (diff*k+8)>>4 recipe as s_interp3_delta[k=6,11].
       Dominant cost on compound 3× BG pages (e.g. test008C p1). */
    for (; x + 8 <= w - 1; x += 8) {
        uint8x8x3_t pa = vld3_u8(src + (size_t)x * 3);
        uint8x8x3_t pb = vld3_u8(src + (size_t)(x + 1) * 3);
        uint8x8_t ar = pa.val[0], ag = pa.val[1], ab = pa.val[2];
        int16x8_t ars = vreinterpretq_s16_u16(vmovl_u8(ar));
        int16x8_t ags = vreinterpretq_s16_u16(vmovl_u8(ag));
        int16x8_t abs16 = vreinterpretq_s16_u16(vmovl_u8(ab));
        int16x8_t dr = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(pb.val[0])), ars);
        int16x8_t dg = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(pb.val[1])), ags);
        int16x8_t db = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(pb.val[2])), abs16);
        int16x8_t bias = vdupq_n_s16(FRACSIZE2);
        int16x8_t d6r = vshrq_n_s16(vaddq_s16(vmulq_n_s16(dr, 6), bias), FRACBITS);
        int16x8_t d6g = vshrq_n_s16(vaddq_s16(vmulq_n_s16(dg, 6), bias), FRACBITS);
        int16x8_t d6b = vshrq_n_s16(vaddq_s16(vmulq_n_s16(db, 6), bias), FRACBITS);
        int16x8_t d11r = vshrq_n_s16(vaddq_s16(vmulq_n_s16(dr, 11), bias), FRACBITS);
        int16x8_t d11g = vshrq_n_s16(vaddq_s16(vmulq_n_s16(dg, 11), bias), FRACBITS);
        int16x8_t d11b = vshrq_n_s16(vaddq_s16(vmulq_n_s16(db, 11), bias), FRACBITS);
        /* mid = a + (d*6+8)>>4 ; far = a + (d*11+8)>>4 (in 0..255 for RGB). */
        uint8x8_t mr = vqmovun_s16(vaddq_s16(ars, d6r));
        uint8x8_t mg = vqmovun_s16(vaddq_s16(ags, d6g));
        uint8x8_t mb = vqmovun_s16(vaddq_s16(abs16, d6b));
        uint8x8_t fr = vqmovun_s16(vaddq_s16(ars, d11r));
        uint8x8_t fg = vqmovun_s16(vaddq_s16(ags, d11g));
        uint8x8_t fb = vqmovun_s16(vaddq_s16(abs16, d11b));
        {
            /* vst3 → interleaved RGB for a/mid/far, then merge a,m,f per pixel. */
            uint8_t abuf[24], mbuf[24], fbuf[24];
            uint8x8x3_t va, vm, vf3;
            int i;
            va.val[0] = ar; va.val[1] = ag; va.val[2] = ab;
            vm.val[0] = mr; vm.val[1] = mg; vm.val[2] = mb;
            vf3.val[0] = fr; vf3.val[1] = fg; vf3.val[2] = fb;
            vst3_u8(abuf, va);
            vst3_u8(mbuf, vm);
            vst3_u8(fbuf, vf3);
            for (i = 0; i < 8; i++) {
                const uint8_t *aa = abuf + i * 3;
                const uint8_t *mm = mbuf + i * 3;
                const uint8_t *ff = fbuf + i * 3;
                d[0] = aa[0]; d[1] = aa[1]; d[2] = aa[2];
                d[3] = mm[0]; d[4] = mm[1]; d[5] = mm[2];
                d[6] = ff[0]; d[7] = ff[1]; d[8] = ff[2];
                d += 9;
            }
        }
    }
#endif
    for (; x < w - 1; x++) {
        const uint8_t *a = src + (size_t)x * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        uint32_t ir = s_interp3_delta[255 + b[0] - ar];
        uint32_t ig = s_interp3_delta[255 + b[1] - ag];
        uint32_t ib = s_interp3_delta[255 + b[2] - ab];

        /* Triple: exact a, then 6/16 and 11/16 toward b (prepare_coord red=3). */
        d[0] = (uint8_t)ar;
        d[1] = (uint8_t)ag;
        d[2] = (uint8_t)ab;
        d[3] = (uint8_t)(ar + (int16_t)ir);
        d[4] = (uint8_t)(ag + (int16_t)ig);
        d[5] = (uint8_t)(ab + (int16_t)ib);
        d[6] = (uint8_t)(ar + (int16_t)(ir >> 16));
        d[7] = (uint8_t)(ag + (int16_t)(ig >> 16));
        d[8] = (uint8_t)(ab + (int16_t)(ib >> 16));
        d += 9;
    }

    last = src + (size_t)(w - 1) * 3;
    for (x = 0; x < ntail; x++) {
        d[0] = last[0];
        d[1] = last[1];
        d[2] = last[2];
        d += 3;
    }
}

static void scaler_interp_row(const uint8_t *lower, const uint8_t *upper,
                              int w, int vf, uint8_t *dst)
{
    int i = 0, n = w * 3;

#ifdef DJVU_SCALER_SSE2
    /* lo + ((up - lo) * vf + 8) >> 4  with 16-bit arithmetic (bytes 0..255). */
    if (n >= 16) {
        const __m128i vvf = _mm_set1_epi16((short)vf);
        const __m128i bias = _mm_set1_epi16(FRACSIZE2);
        for (; i + 16 <= n; i += 16) {
            __m128i lo8 = _mm_loadu_si128((const __m128i *)(lower + i));
            __m128i up8 = _mm_loadu_si128((const __m128i *)(upper + i));
            __m128i lo_lo = _mm_unpacklo_epi8(lo8, _mm_setzero_si128());
            __m128i lo_hi = _mm_unpackhi_epi8(lo8, _mm_setzero_si128());
            __m128i up_lo = _mm_unpacklo_epi8(up8, _mm_setzero_si128());
            __m128i up_hi = _mm_unpackhi_epi8(up8, _mm_setzero_si128());
            __m128i d_lo = _mm_sub_epi16(up_lo, lo_lo);
            __m128i d_hi = _mm_sub_epi16(up_hi, lo_hi);
            d_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(d_lo, vvf), bias),
                                  FRACBITS);
            d_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(d_hi, vvf), bias),
                                  FRACBITS);
            _mm_storeu_si128((__m128i *)(dst + i),
                             _mm_packus_epi16(_mm_add_epi16(lo_lo, d_lo),
                                              _mm_add_epi16(lo_hi, d_hi)));
        }
    }
#endif
#ifdef DJVU_SCALER_NEON
    if (n >= 16) {
        const int16x8_t vvf = vdupq_n_s16((int16_t)vf);
        const int16x8_t bias = vdupq_n_s16(FRACSIZE2);
        for (; i + 16 <= n; i += 16) {
            uint8x16_t lo8 = vld1q_u8(lower + i);
            uint8x16_t up8 = vld1q_u8(upper + i);
            int16x8_t lo_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(lo8)));
            int16x8_t lo_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(lo8)));
            int16x8_t up_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(up8)));
            int16x8_t up_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(up8)));
            int16x8_t d_lo = vsubq_s16(up_lo, lo_lo);
            int16x8_t d_hi = vsubq_s16(up_hi, lo_hi);
            d_lo = vshrq_n_s16(vmlaq_s16(bias, d_lo, vvf), FRACBITS);
            d_hi = vshrq_n_s16(vmlaq_s16(bias, d_hi, vvf), FRACBITS);
            vst1q_u8(dst + i, vcombine_u8(vqmovun_s16(vaddq_s16(lo_lo, d_lo)),
                                          vqmovun_s16(vaddq_s16(lo_hi, d_hi))));
        }
    }
#endif
    for (; i < n; i++) {
        int lo = lower[i];
        dst[i] = (uint8_t)(lo + (((upper[i] - lo) * vf + FRACSIZE2) >> FRACBITS));
    }
}

/* Ceil-3x upscale: outw in [3*inw-2, 3*inw], outh in [3*inh-2, 3*inh].
   Row sequence mirrors prepare_coord(red=3) vertically: row 0 = input row 0,
   then per input row pair the (0, 6/16, 11/16) interpolated triple, then the
   last input row replicated for the outh-(3h-2) trailing clamped rows. */
static int scaler_scale_red3_into(scaler *s, const djvu_cpix *in,
                                  uint8_t *dst0, int stride, int topdown)
{
    djvu_ctx *ctx = s->ctx;
    int w = s->inw, h = s->inh, outw = s->outw;
    size_t row = (size_t)w * 3;
    uint8_t *tmp;
    const uint8_t *last;
    int y, vtail = s->outh - (3 * h - 2);

    tmp = (uint8_t *)djvu_alloc(ctx, row);
    if (!tmp) return -1;

    scaler_expand_row3(in->d, w, outw,
                       scaler_dest_row(dst0, stride, s->outh, topdown, 0));
    for (y = 0; y < h - 1; y++) {
        const uint8_t *lower = in->d + (size_t)y * row;
        const uint8_t *upper = lower + row;

        scaler_expand_row3(lower, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           y * 3 + 1));
        scaler_interp_row(lower, upper, w, 6, tmp);
        scaler_expand_row3(tmp, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           y * 3 + 2));
        scaler_interp_row(lower, upper, w, 11, tmp);
        scaler_expand_row3(tmp, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           y * 3 + 3));
    }

    last = in->d + (size_t)(h - 1) * row;
    for (y = 0; y < vtail; y++)
        scaler_expand_row3(last, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           h * 3 - 2 + y));
    djvu_free(ctx, tmp);
    return 0;
}

/* Horizontal bilinear expansion for integer scales. Output coordinates are
   monotonic, so keep each source pair in registers for all of its destination
   samples instead of reloading the pair through hinfo for every pixel. */
static void scaler_expand_row_grouped(const uint8_t *src, int w,
                                      const int *coord, int outw, uint8_t *dst)
{
    int x = 0, sx;
    const uint8_t *last = src + (size_t)(w - 1) * 3;

    while (x < outw && coord[x] < 0) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst += 3;
        x++;
    }
    for (sx = 0; sx < w - 1 && x < outw; sx++) {
        const uint8_t *a = src + (size_t)sx * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        int dr = b[0] - ar, dg = b[1] - ag, db = b[2] - ab;

        while (x < outw && (coord[x] >> FRACBITS) == sx) {
            int hf = coord[x] & FRACMASK;
            dst[0] = (uint8_t)(ar + ((dr * hf + FRACSIZE2) >> FRACBITS));
            dst[1] = (uint8_t)(ag + ((dg * hf + FRACSIZE2) >> FRACBITS));
            dst[2] = (uint8_t)(ab + ((db * hf + FRACSIZE2) >> FRACBITS));
            dst += 3;
            x++;
        }
    }
    while (x < outw) {
        dst[0] = last[0]; dst[1] = last[1]; dst[2] = last[2];
        dst += 3;
        x++;
    }
}

/* The prepare_coord phase cycles for exact 4x and 12x expansion. These are
   common DjVu background reductions; unrolling full source-pair groups removes
   the coordinate load, shift, and loop-exit test from every output pixel. */
static void scaler_expand_row4(const uint8_t *src, int w, int outw, uint8_t *dst)
{
    static const uint8_t phase[4] = { 2, 6, 10, 14 };
    int x = 0, sx, k, groups;
    const uint8_t *last = src + (size_t)(w - 1) * 3;

    while (x < outw && x < 2) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst += 3; x++;
    }
    groups = (outw - x) / 4;
    if (groups > w - 1) groups = w - 1;
    for (sx = 0; sx < groups; sx++) {
        const uint8_t *a = src + (size_t)sx * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        int dr = b[0] - ar, dg = b[1] - ag, db = b[2] - ab;
#define EMIT4(F) \
        dst[0] = (uint8_t)(ar + ((dr * (F) + FRACSIZE2) >> FRACBITS)); \
        dst[1] = (uint8_t)(ag + ((dg * (F) + FRACSIZE2) >> FRACBITS)); \
        dst[2] = (uint8_t)(ab + ((db * (F) + FRACSIZE2) >> FRACBITS)); \
        dst += 3
        EMIT4(2); EMIT4(6); EMIT4(10); EMIT4(14);
#undef EMIT4
    }
    x += groups * 4;
    if (groups < w - 1) {
        const uint8_t *a = src + (size_t)groups * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        int dr = b[0] - ar, dg = b[1] - ag, db = b[2] - ab;
        for (k = 0; k < 4 && x < outw; k++, x++) {
            int hf = phase[k];
            dst[0] = (uint8_t)(ar + ((dr * hf + FRACSIZE2) >> FRACBITS));
            dst[1] = (uint8_t)(ag + ((dg * hf + FRACSIZE2) >> FRACBITS));
            dst[2] = (uint8_t)(ab + ((db * hf + FRACSIZE2) >> FRACBITS));
            dst += 3;
        }
    }
    while (x++ < outw) {
        dst[0] = last[0]; dst[1] = last[1]; dst[2] = last[2];
        dst += 3;
    }
}

static void scaler_expand_row12(const uint8_t *src, int w, int outw, uint8_t *dst)
{
    static const uint8_t phase[12] =
        { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
    int x = 0, sx, k, groups;
    const uint8_t *last = src + (size_t)(w - 1) * 3;

    while (x < outw && x < 5) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst += 3; x++;
    }
    groups = (outw - x) / 12;
    if (groups > w - 1) groups = w - 1;
    for (sx = 0; sx < groups; sx++) {
        const uint8_t *a = src + (size_t)sx * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        int dr = b[0] - ar, dg = b[1] - ag, db = b[2] - ab;
#define EMIT12(F) \
        dst[0] = (uint8_t)(ar + ((dr * (F) + FRACSIZE2) >> FRACBITS)); \
        dst[1] = (uint8_t)(ag + ((dg * (F) + FRACSIZE2) >> FRACBITS)); \
        dst[2] = (uint8_t)(ab + ((db * (F) + FRACSIZE2) >> FRACBITS)); \
        dst += 3
        EMIT12(0);  EMIT12(1);  EMIT12(2);  EMIT12(4);
        EMIT12(5);  EMIT12(6);  EMIT12(8);  EMIT12(9);
        EMIT12(10); EMIT12(12); EMIT12(13); EMIT12(14);
#undef EMIT12
    }
    x += groups * 12;
    if (groups < w - 1) {
        const uint8_t *a = src + (size_t)groups * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        int dr = b[0] - ar, dg = b[1] - ag, db = b[2] - ab;
        for (k = 0; k < 12 && x < outw; k++, x++) {
            int hf = phase[k];
            dst[0] = (uint8_t)(ar + ((dr * hf + FRACSIZE2) >> FRACBITS));
            dst[1] = (uint8_t)(ag + ((dg * hf + FRACSIZE2) >> FRACBITS));
            dst[2] = (uint8_t)(ab + ((db * hf + FRACSIZE2) >> FRACBITS));
            dst += 3;
        }
    }
    while (x++ < outw) {
        dst[0] = last[0]; dst[1] = last[1]; dst[2] = last[2];
        dst += 3;
    }
}

static int scaler_scale_into(scaler *s, const djvu_cpix *in,
                             uint8_t *dst0, int stride, int topdown)
{
    djvu_ctx *ctx = s->ctx;
    int bufw, y, grouped;
    uint8_t *lbuf;

    if (!in || !in->d || in->w <= 0 || in->h <= 0 ||
        in->w != s->inw || in->h != s->inh)
        return -1;
    uint8_t *p1 = NULL, *p2 = NULL; int l1 = -1, l2 = -1;
    int red_xmin = 0, red_xmax = s->redw;
    uint16_t *hinfo16 = NULL;
    uint32_t *hinfo32 = NULL;

    prepare_interp();
    if (!s->hcoord) scaler_set_h(s, 0, 0);
    if (!s->vcoord) scaler_set_v(s, 0, 0);
    if (s->xshift == 0 && s->yshift == 0 &&
        s->hnum == 3 && s->hden == 1 && s->vnum == 3 && s->vden == 1 &&
        s->inw > 0 && s->inh > 0 &&
        s->redw == s->inw && s->redh == s->inh &&
        s->outw >= 3 * s->inw - 2 && s->outw <= 3 * s->inw &&
        s->outh >= 3 * s->inh - 2 && s->outh <= 3 * s->inh &&
        in->w == s->inw && in->h == s->inh)
        return scaler_scale_red3_into(s, in, dst0, stride, topdown);
    grouped = s->xshift == 0 && s->hden == 1 && s->hnum >= 4;
    bufw = s->redw;
    lbuf = (uint8_t *)djvu_alloc(ctx, (size_t)(bufw + 2) * 3);
    if (!lbuf) return -1;
    if (s->xshift > 0 || s->yshift > 0) {
        p1 = (uint8_t *)djvu_alloc(ctx, (size_t)bufw * 3);
        p2 = (uint8_t *)djvu_alloc(ctx, (size_t)bufw * 3);
        if (!p1 || !p2) { djvu_free(ctx, lbuf); djvu_free(ctx, p1); djvu_free(ctx, p2); return -1; }
    }
    if (red_xmin == 0 && !grouped) {
        int x;
        int use16 = (s->redw * 3) <= 0x0fff;
        if (use16)
            hinfo16 = (uint16_t *)djvu_alloc(ctx, sizeof(uint16_t) * s->outw);
        else
            hinfo32 = (uint32_t *)djvu_alloc(ctx, sizeof(uint32_t) * s->outw);
        if ((use16 && !hinfo16) || (!use16 && !hinfo32)) {
            djvu_free(ctx, hinfo16);
            djvu_free(ctx, hinfo32);
            djvu_free(ctx, lbuf);
            djvu_free(ctx, p1);
            djvu_free(ctx, p2);
            return -1;
        }
        if (use16) {
            for (x = 0; x < s->outw; x++) {
                int n = s->hcoord[x];
                int off = (1 + (n >> FRACBITS)) * 3;
                hinfo16[x] = (uint16_t)((off << FRACBITS) | (n & FRACMASK));
            }
        } else {
            for (x = 0; x < s->outw; x++) {
                int n = s->hcoord[x];
                int off = (1 + (n >> FRACBITS)) * 3;
                hinfo32[x] = (uint32_t)((off << FRACBITS) | (n & FRACMASK));
            }
        }
    }

    for (y = 0; y < s->outh; y++) {
        int fy = s->vcoord[y];
        int fy1 = fy >> FRACBITS, fy2 = fy1 + 1;
        const uint8_t *lower, *upper;
        uint8_t *dest;
        int x;
        int vf;

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
            if (fy1 < 0) fy1 = 0;
            if (fy2 < 0) fy2 = 0;
            if (fy1 >= in->h) fy1 = in->h - 1;
            if (fy2 >= in->h) fy2 = in->h - 1;
            if (fy1 > s->redh - 1) fy1 = s->redh - 1;
            if (fy2 > s->redh - 1) fy2 = s->redh - 1;
            lower = in->d + (size_t)fy1 * in->w * 3;
            upper = in->d + (size_t)fy2 * in->w * 3;
        }
        vf = fy & FRACMASK;
        for (x = 0; x < bufw; x++) {
            int lr = lower[x*3+0], lg = lower[x*3+1], lb = lower[x*3+2];
            lbuf[(x+1)*3+0] = (uint8_t)(lr + (((upper[x*3+0] - lr) * vf + FRACSIZE2) >> FRACBITS));
            lbuf[(x+1)*3+1] = (uint8_t)(lg + (((upper[x*3+1] - lg) * vf + FRACSIZE2) >> FRACBITS));
            lbuf[(x+1)*3+2] = (uint8_t)(lb + (((upper[x*3+2] - lb) * vf + FRACSIZE2) >> FRACBITS));
        }
        lbuf[0]=lbuf[3]; lbuf[1]=lbuf[4]; lbuf[2]=lbuf[5];
        lbuf[(bufw+1)*3+0]=lbuf[bufw*3+0]; lbuf[(bufw+1)*3+1]=lbuf[bufw*3+1]; lbuf[(bufw+1)*3+2]=lbuf[bufw*3+2];
        dest = dst0 + (size_t)(topdown ? (s->outh - 1 - y) : y) * stride;
        if (grouped) {
            if (s->hnum == 12)
                scaler_expand_row12(lbuf + 3, bufw, s->outw, dest);
            else if (s->hnum == 4)
                scaler_expand_row4(lbuf + 3, bufw, s->outw, dest);
            else
                scaler_expand_row_grouped(lbuf + 3, bufw, s->hcoord,
                                          s->outw, dest);
        } else if (hinfo16) {
            uint8_t *dp = dest;
            for (x = 0; x < s->outw; x++) {
                int n = hinfo16[x];
                const uint8_t *lo = lbuf + (n >> FRACBITS);
                int hf = n & FRACMASK;
                int lr = lo[0], lg = lo[1], lb = lo[2];
                int dr = lo[3] - lr, dg = lo[4] - lg, db = lo[5] - lb;
                dp[0] = (uint8_t)(lr + ((dr * hf + FRACSIZE2) >> FRACBITS));
                dp[1] = (uint8_t)(lg + ((dg * hf + FRACSIZE2) >> FRACBITS));
                dp[2] = (uint8_t)(lb + ((db * hf + FRACSIZE2) >> FRACBITS));
                dp += 3;
            }
        } else if (hinfo32) {
            uint8_t *dp = dest;
            for (x = 0; x < s->outw; x++) {
                int n = (int)hinfo32[x];
                const uint8_t *lo = lbuf + (n >> FRACBITS);
                int hf = n & FRACMASK;
                int lr = lo[0], lg = lo[1], lb = lo[2];
                int dr = lo[3] - lr, dg = lo[4] - lg, db = lo[5] - lb;
                dp[0] = (uint8_t)(lr + ((dr * hf + FRACSIZE2) >> FRACBITS));
                dp[1] = (uint8_t)(lg + ((dg * hf + FRACSIZE2) >> FRACBITS));
                dp[2] = (uint8_t)(lb + ((db * hf + FRACSIZE2) >> FRACBITS));
                dp += 3;
            }
        } else {
            uint8_t *dp = dest;
            for (x = 0; x < s->outw; x++) {
                int n = s->hcoord[x];
                const uint8_t *lo = lbuf + (1 + (n >> FRACBITS) - red_xmin) * 3;
                int hf = n & FRACMASK;
                int lr = lo[0], lg = lo[1], lb = lo[2];
                dp[0] = (uint8_t)(lr + (((lo[3] - lr) * hf + FRACSIZE2) >> FRACBITS));
                dp[1] = (uint8_t)(lg + (((lo[4] - lg) * hf + FRACSIZE2) >> FRACBITS));
                dp[2] = (uint8_t)(lb + (((lo[5] - lb) * hf + FRACSIZE2) >> FRACBITS));
                dp += 3;
            }
        }
    }
    djvu_free(ctx, hinfo16);
    djvu_free(ctx, hinfo32);
    djvu_free(ctx, lbuf); djvu_free(ctx, p1); djvu_free(ctx, p2);
    return 0;
}

static int scaler_scale(scaler *s, const djvu_cpix *in, djvu_cpix *out)
{
    if (cpix_init_uninit(s->ctx, out, s->outw, s->outh) != 0) return -1;
    return scaler_scale_into(s, in, out->d, s->outw * 3, 0);
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
    return djvu_cpix_scale_ratio(ctx, in, out, outw, outh, red, 1);
}

/* Scale by an arbitrary rational factor: output = input * numer/denom.
   Generalizes djvu_cpix_scale (whose upscale factor `red` is numer/1) so a
   reduced IW44 layer (reduction `red` vs the page) can be scaled straight to a
   subsampled target with numer=red, denom=subsample -- including fractional
   ratios (red=3, subsample=2) and downscales (subsample > red, handled by the
   scaler's power-of-two pre-reduction). */
int djvu_cpix_scale_ratio(djvu_ctx *ctx, const djvu_cpix *in, djvu_cpix *out,
                          int outw, int outh, int numer, int denom)
{
    scaler s;
    memset(&s, 0, sizeof(s));
    s.ctx = ctx;
    s.inw = in->w;
    s.inh = in->h;
    s.outw = outw;
    s.outh = outh;
    scaler_set_h(&s, numer, denom);
    scaler_set_v(&s, numer, denom);
    if (scaler_scale(&s, in, out) != 0) { scaler_free(&s); return -1; }
    scaler_free(&s);
    return 0;
}

int djvu_cpix_scale_to_topdown_rgb(djvu_ctx *ctx, const djvu_cpix *in,
                                   uint8_t *dst, int stride,
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
    if (scaler_scale_into(&s, in, dst, stride, 1) != 0) {
        scaler_free(&s);
        return -1;
    }
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
