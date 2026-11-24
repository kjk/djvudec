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

static int scaler_set_h(scaler *s, int numer, int denom)
{
    s->hnum = numer; s->hden = denom;
    s->xshift = 0; s->redw = s->inw;
    while (numer + numer < denom) { s->xshift++; s->redw = (s->redw + 1) >> 1; numer <<= 1; }
    s->hcoord = (int *)djvu_alloc(s->ctx, sizeof(int) * s->outw);
    if (!s->hcoord) return -1;
    prepare_coord(s->hcoord, s->redw, s->outw, denom, numer);
    return 0;
}
static int scaler_set_v(scaler *s, int numer, int denom)
{
    s->vnum = numer; s->vden = denom;
    s->yshift = 0; s->redh = s->inh;
    while (numer + numer < denom) { s->yshift++; s->redh = (s->redh + 1) >> 1; numer <<= 1; }
    s->vcoord = (int *)djvu_alloc(s->ctx, sizeof(int) * s->outh);
    if (!s->vcoord) return -1;
    prepare_coord(s->vcoord, s->redh, s->outh, denom, numer);
    return 0;
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

static uint8_t *scaler_dest_row(uint8_t *dst0, int stride, int outh,
                                int topdown, int y)
{
    return dst0 + (size_t)(topdown ? (outh - 1 - y) : y) * stride;
}

#ifdef DJVU_SCALER_NEON
static uint16x8_t scaler_interp_sum_neon(uint8x8_t a, uint8x8_t b,
                                         uint16_t phase)
{
    return vmlaq_n_u16(vshll_n_u8(a, FRACBITS), vsubl_u8(b, a), phase);
}
#endif

#if defined(DJVU_SCALER_NEON) && (defined(__aarch64__) || defined(__arm64__))
#define DJVU_SCALER_NEON_TABLE 1

static uint8x16_t scaler_round_interp(uint16x8_t lo, uint16x8_t hi)
{
    return vrshrn_high_n_u16(vrshrn_n_u16(lo, FRACBITS), hi, FRACBITS);
}

/* Compute ((16 - phase) * a + phase * b + 8) >> 4 for each byte.
   The unsigned difference can wrap; the weighted sum is always 0..4080. */
static uint8x16_t scaler_interp_neon(uint8x16_t a, uint8x16_t b,
                                    uint16_t phase)
{
    uint16x8_t dhi = vsubl_high_u8(b, a);
    return scaler_round_interp(
        scaler_interp_sum_neon(vget_low_u8(a), vget_low_u8(b), phase),
        vmlaq_n_u16(vshll_high_n_u8(a, FRACBITS), dhi, phase));
}

static uint8x16_t scaler_interp_phases_neon(uint8x16_t a, uint8x16_t b,
                                           uint8x16_t phase)
{
    uint8x16_t inv = vsubq_u8(vdupq_n_u8(FRACSIZE), phase);
    return scaler_round_interp(
        vmlal_u8(vmull_u8(vget_low_u8(a), vget_low_u8(inv)),
                 vget_low_u8(b), vget_low_u8(phase)),
        vmlal_high_u8(vmull_high_u8(a, inv), b, phase));
}
#endif

static void scaler_interp_row(const uint8_t *lower, const uint8_t *upper,
                              int w, int vf, uint8_t *dst);
#ifndef DJVU_SCALER_NEON_TABLE   /* unused where the NEON table kernels apply */
static void scaler_expand_periodic(const uint8_t *src, int w, int outw,
                                   int head, const uint8_t *phase, int nphase,
                                   uint8_t *dst, uint8_t *scratch);
#endif

/* Bytes of scratch scaler_expand_periodic needs for a w-pixel source row. */
static size_t expand_scratch_size(int w, int nphase)
{
    return (size_t)nphase * ((size_t)w * 3 + 4);
}

/* Expand one row w -> outw for the ceil-3x upscale (outw in [3w-2, 3w]).
   Emits the exact prepare_coord(red=3) sample sequence: src[0] once, then
   per input pixel the (0, 6/16, 11/16) interpolation triple, then the last
   pixel replicated outw-(3w-2) more times (the trailing clamped samples). */
static void scaler_expand_row3(const uint8_t *src, int w, int outw, uint8_t *dst,
                               uint8_t *scratch)
{
#ifndef DJVU_SCALER_NEON
    /* x86 (and anything without NEON): per-phase SIMD lerp + interleave */
    static const uint8_t phase[3] = { 0, 6, 11 };
    scaler_expand_periodic(src, w, outw, 1, phase, 3, dst, scratch);
#else
    const uint8_t *last;
    uint8_t *d = dst;
    int x, ntail = outw - (3 * w - 2);

    d[0] = src[0];
    d[1] = src[1];
    d[2] = src[2];
    d += 3;

    x = 0;
#ifdef DJVU_SCALER_NEON_TABLE
    /* Each table vector holds one phase in RGB byte order. Gather five
       triples (45 bytes), overlapping bytes 29..31 in the final store.
       Both 16-byte loads fit when at least seven input pixels remain. */
    const uint8x16_t first =
        { 0, 1, 2, 16, 17, 18, 32, 33, 34, 3, 4, 5, 19, 20, 21, 35 };
    const uint8x16_t second =
        { 36, 37, 6, 7, 8, 22, 23, 24, 38, 39, 40, 9, 10, 11, 25, 26 };
    const uint8x16_t third =
        { 11, 25, 26, 27, 41, 42, 43, 12, 13, 14, 28, 29, 30, 44, 45, 46 };
    for (; x + 7 <= w; x += 5, d += 45) {
        const uint8_t *s = src + (size_t)x * 3;
        uint8x16_t a = vld1q_u8(s);
        uint8x16_t b = vld1q_u8(s + 3);
        uint8x16x3_t phases = { { a, scaler_interp_neon(a, b, 6),
                                  scaler_interp_neon(a, b, 11) } };
        vst1q_u8(d, vqtbl3q_u8(phases, first));
        vst1q_u8(d + 16, vqtbl3q_u8(phases, second));
        vst1q_u8(d + 29, vqtbl3q_u8(phases, third));
    }
#elif defined(DJVU_SCALER_NEON)
    /* The 16-lane table form is unavailable on 32-bit Arm. Gather two
       triples (18 bytes) from the narrower phase vectors. */
    const uint8x8_t first = { 0, 1, 2, 8, 9, 10, 16, 17 };
    const uint8x8_t second = { 18, 3, 4, 5, 11, 12, 13, 19 };
    const uint8x8_t third = { 4, 5, 11, 12, 13, 19, 20, 21 };
    for (; x + 4 <= w; x += 2, d += 18) {
        const uint8_t *s = src + (size_t)x * 3;
        uint8x8_t a = vld1_u8(s);
        uint8x8_t b = vld1_u8(s + 3);
        uint8x8x3_t phases = {
            { a, vrshrn_n_u16(scaler_interp_sum_neon(a, b, 6), FRACBITS),
                 vrshrn_n_u16(scaler_interp_sum_neon(a, b, 11), FRACBITS) }
        };
        vst1_u8(d, vtbl3_u8(phases, first));
        vst1_u8(d + 8, vtbl3_u8(phases, second));
        vst1_u8(d + 10, vtbl3_u8(phases, third));
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
    (void)scratch;
#endif
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
#ifdef DJVU_SCALER_NEON_TABLE
    for (; i + 16 <= n; i += 16)
        vst1q_u8(dst + i, scaler_interp_neon(vld1q_u8(lower + i),
                                             vld1q_u8(upper + i), (uint16_t)vf));
#elif defined(DJVU_SCALER_NEON)
    for (; i + 8 <= n; i += 8)
        vst1_u8(dst + i, vrshrn_n_u16(
            scaler_interp_sum_neon(vld1_u8(lower + i), vld1_u8(upper + i),
                                   (uint16_t)vf), FRACBITS));
#endif
    for (; i < n; i++) {
        int lo = lower[i];
        dst[i] = (uint8_t)(lo + (((upper[i] - lo) * vf + FRACSIZE2) >> FRACBITS));
    }
}

#ifndef DJVU_SCALER_NEON_TABLE
/* Horizontal bilinear expansion by an integer factor whose prepare_coord
   phases repeat for every source pair: `head` copies of src[0], then for
   each source pair (a, b) the outputs a + ((b - a) * phase[k] + 8) >> 4 for
   k = 0..nphase-1, then the last pixel replicated up to outw (the clamped
   trailing samples). Same arithmetic as the scalar loops it replaces.
   Each phase is computed for the whole row with scaler_interp_row (SSE2 /
   NEON); the phases are then interleaved into dst with 4-byte copies (each
   copy's 4th byte is overwritten by the next output, the last output uses a
   3-byte copy so nothing past the row is written). Previously every output
   byte cost its own multiply, add and shift.
   scratch: expand_scratch_size(w, nphase) bytes. */
static void scaler_expand_periodic(const uint8_t *src, int w, int outw,
                                   int head, const uint8_t *phase, int nphase,
                                   uint8_t *dst, uint8_t *scratch)
{
    const uint8_t *pp[16];
    const uint8_t *last = src + (size_t)(w - 1) * 3;
    uint8_t *d = dst;
    int x = 0, k, sx, full, npairs = w - 1;

    while (x < outw && x < head) {
        d[0] = src[0]; d[1] = src[1]; d[2] = src[2];
        d += 3; x++;
    }
    full = npairs > 0 ? (outw - x) / nphase : 0;
    if (full > npairs) full = npairs;
    if (full > 0) {
        size_t plen = (size_t)full * 3;
        uint8_t *sp = scratch;
        for (k = 0; k < nphase; k++) {
            if (phase[k] == 0) {
                pp[k] = src;              /* a itself */
            } else {
                scaler_interp_row(src, src + 3, full, phase[k], sp);
                sp[plen] = 0;             /* read (never kept) by the 4-byte copy */
                pp[k] = sp;
                sp += plen + 4;
            }
        }
        for (sx = 0; sx < full - 1; sx++) {
            size_t o = (size_t)sx * 3;
            for (k = 0; k < nphase; k++) {
                memcpy(d, pp[k] + o, 4);
                d += 3;
            }
        }
        {
            size_t o = (size_t)(full - 1) * 3;
            for (k = 0; k < nphase - 1; k++) {
                memcpy(d, pp[k] + o, 4);
                d += 3;
            }
            memcpy(d, pp[nphase - 1] + o, 3);
            d += 3;
        }
        x += full * nphase;
    }
    if (full < npairs) {                  /* partial last group */
        const uint8_t *a = src + (size_t)full * 3;
        const uint8_t *b = a + 3;
        int ar = a[0], ag = a[1], ab = a[2];
        int dr = b[0] - ar, dg = b[1] - ag, db = b[2] - ab;
        for (k = 0; k < nphase && x < outw; k++, x++) {
            int hf = phase[k];
            d[0] = (uint8_t)(ar + ((dr * hf + FRACSIZE2) >> FRACBITS));
            d[1] = (uint8_t)(ag + ((dg * hf + FRACSIZE2) >> FRACBITS));
            d[2] = (uint8_t)(ab + ((db * hf + FRACSIZE2) >> FRACBITS));
            d += 3;
        }
    }
    while (x < outw) {
        d[0] = last[0]; d[1] = last[1]; d[2] = last[2];
        d += 3; x++;
    }
}
#endif /* !DJVU_SCALER_NEON_TABLE */

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
    uint8_t *tmp, *scratch;
    const uint8_t *last;
    int y, vtail = s->outh - (3 * h - 2);

    tmp = (uint8_t *)djvu_alloc(ctx, row);
    scratch = (uint8_t *)djvu_alloc(ctx, expand_scratch_size(w, 3));
    if (!tmp || !scratch) { djvu_free(ctx, tmp); djvu_free(ctx, scratch); return -1; }

    scaler_expand_row3(in->d, w, outw,
                       scaler_dest_row(dst0, stride, s->outh, topdown, 0), scratch);
    for (y = 0; y < h - 1; y++) {
        const uint8_t *lower = in->d + (size_t)y * row;
        const uint8_t *upper;
        if ((y & 31) == 0 && djvu_aborted(ctx)) {   /* 3 output rows per y */
            djvu_free(ctx, tmp);
            djvu_free(ctx, scratch);
            return -1;
        }
        upper = lower + row;

        scaler_expand_row3(lower, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           y * 3 + 1), scratch);
        scaler_interp_row(lower, upper, w, 6, tmp);
        scaler_expand_row3(tmp, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           y * 3 + 2), scratch);
        scaler_interp_row(lower, upper, w, 11, tmp);
        scaler_expand_row3(tmp, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           y * 3 + 3), scratch);
    }

    last = in->d + (size_t)(h - 1) * row;
    for (y = 0; y < vtail; y++)
        scaler_expand_row3(last, w, outw,
                           scaler_dest_row(dst0, stride, s->outh, topdown,
                                           h * 3 - 2 + y), scratch);
    djvu_free(ctx, tmp);
    djvu_free(ctx, scratch);
    return 0;
}

/* dst[i] = a[i] + (((b[i] - a[i]) * f[i] + 8) >> 4): the horizontal lerp with
   a per-byte phase (int16 math, arithmetic shift, results in 0..255 -- same
   as the scalar expression). */
static void scaler_lerp_bytes(const uint8_t *a, const uint8_t *b, const uint8_t *f,
                              size_t n, uint8_t *dst)
{
    size_t i = 0;
#ifdef DJVU_SCALER_SSE2
    {
        const __m128i z = _mm_setzero_si128();
        const __m128i bias = _mm_set1_epi16(FRACSIZE2);
        for (; i + 16 <= n; i += 16) {
            __m128i va = _mm_loadu_si128((const __m128i *)(a + i));
            __m128i vb = _mm_loadu_si128((const __m128i *)(b + i));
            __m128i vf = _mm_loadu_si128((const __m128i *)(f + i));
            __m128i alo = _mm_unpacklo_epi8(va, z), ahi = _mm_unpackhi_epi8(va, z);
            __m128i dlo = _mm_sub_epi16(_mm_unpacklo_epi8(vb, z), alo);
            __m128i dhi = _mm_sub_epi16(_mm_unpackhi_epi8(vb, z), ahi);
            dlo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(dlo, _mm_unpacklo_epi8(vf, z)), bias), FRACBITS);
            dhi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(dhi, _mm_unpackhi_epi8(vf, z)), bias), FRACBITS);
            _mm_storeu_si128((__m128i *)(dst + i),
                             _mm_packus_epi16(_mm_add_epi16(alo, dlo), _mm_add_epi16(ahi, dhi)));
        }
    }
#endif
#ifdef DJVU_SCALER_NEON
    {
        const int16x8_t bias = vdupq_n_s16(FRACSIZE2);
        for (; i + 16 <= n; i += 16) {
            uint8x16_t va = vld1q_u8(a + i), vb = vld1q_u8(b + i), vf = vld1q_u8(f + i);
            int16x8_t alo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(va)));
            int16x8_t ahi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(va)));
            int16x8_t dlo = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vb))), alo);
            int16x8_t dhi = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(vb))), ahi);
            int16x8_t flo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(vf)));
            int16x8_t fhi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(vf)));
            dlo = vshrq_n_s16(vmlaq_s16(bias, dlo, flo), FRACBITS);
            dhi = vshrq_n_s16(vmlaq_s16(bias, dhi, fhi), FRACBITS);
            vst1q_u8(dst + i, vcombine_u8(vqmovun_s16(vaddq_s16(alo, dlo)),
                                          vqmovun_s16(vaddq_s16(ahi, dhi))));
        }
    }
#endif
    for (; i < n; i++) {
        int lo = a[i];
        dst[i] = (uint8_t)(lo + (((b[i] - lo) * f[i] + FRACSIZE2) >> FRACBITS));
    }
}

/* The prepare_coord phase cycles for exact 4x and 12x expansion. These are
   common DjVu background reductions; unrolling full source-pair groups removes
   the coordinate load, shift, and loop-exit test from every output pixel. */
static void scaler_expand_row4(const uint8_t *src, int w, int outw, uint8_t *dst,
                               uint8_t *scratch)
{
#ifndef DJVU_SCALER_NEON_TABLE
    /* no 16-lane table lookup here: per-phase SIMD lerp + interleave */
    static const uint8_t pphase[4] = { 2, 6, 10, 14 };
    scaler_expand_periodic(src, w, outw, 2, pphase, 4, dst, scratch);
#else
    static const uint8_t phase[4] = { 2, 6, 10, 14 };
    int x = 0, sx, k, groups;
    const uint8_t *last = src + (size_t)(w - 1) * 3;

    while (x < outw && x < 2) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst += 3; x++;
    }
    groups = (outw - x) / 4;
    if (groups > w - 1) groups = w - 1;
    sx = 0;
#ifdef DJVU_SCALER_NEON_TABLE
    /* Gather four RGB source pairs at the four interpolation phases.
       As in the 3x loop, the shifted 16-byte load needs seven input pixels. */
    const uint8x16_t first =
        { 0, 1, 2, 16, 17, 18, 32, 33, 34, 48, 49, 50, 3, 4, 5, 19 };
    const uint8x16_t second =
        { 20, 21, 35, 36, 37, 51, 52, 53, 6, 7, 8, 22, 23, 24, 38, 39 };
    const uint8x16_t third =
        { 40, 54, 55, 56, 9, 10, 11, 25, 26, 27, 41, 42, 43, 57, 58, 59 };
    for (; sx + 4 <= groups && sx + 7 <= w; sx += 4, dst += 48) {
        const uint8_t *s = src + (size_t)sx * 3;
        uint8x16_t a = vld1q_u8(s);
        uint8x16_t b = vld1q_u8(s + 3);
        uint8x16x4_t phases = { { scaler_interp_neon(a, b, 2),
                                  scaler_interp_neon(a, b, 6),
                                  scaler_interp_neon(a, b, 10),
                                  scaler_interp_neon(a, b, 14) } };
        vst1q_u8(dst, vqtbl4q_u8(phases, first));
        vst1q_u8(dst + 16, vqtbl4q_u8(phases, second));
        vst1q_u8(dst + 32, vqtbl4q_u8(phases, third));
    }
#endif
    for (; sx < groups; sx++) {
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
    (void)scratch;
#endif
}

static void scaler_expand_row12(const uint8_t *src, int w, int outw, uint8_t *dst,
                               uint8_t *scratch)
{
#ifndef DJVU_SCALER_NEON_TABLE
    /* no 16-lane table lookup here: per-phase SIMD lerp + interleave */
    static const uint8_t pphase[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
    scaler_expand_periodic(src, w, outw, 5, pphase, 12, dst, scratch);
#else
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
    sx = 0;
#ifdef DJVU_SCALER_NEON_TABLE
    /* Repeat each three-byte pixel across the vector. Each phase vector
       emits four RGB pixels; the first two stores can overlap their successor. */
    const uint8x16_t rgb = { 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2 };
    const uint8x16_t first = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 4, 4, 4 };
    const uint8x16_t second = { 5, 5, 5, 6, 6, 6, 8, 8, 8, 9, 9, 9 };
    const uint8x16_t third = { 10, 10, 10, 12, 12, 12, 13, 13, 13, 14, 14, 14 };
    for (; sx < groups && sx + 2 < w; sx++, dst += 36) {
        uint8x8_t bytes = vld1_u8(src + (size_t)sx * 3);
        uint8x16_t pair = vcombine_u8(bytes, bytes);
        uint8x16_t a = vqtbl1q_u8(pair, rgb);
        uint8x16_t b = vqtbl1q_u8(pair, vaddq_u8(rgb, vdupq_n_u8(3)));
        uint8x16_t tail = scaler_interp_phases_neon(a, b, third);
        uint32_t last_word = vgetq_lane_u32(vreinterpretq_u32_u8(tail), 2);
        vst1q_u8(dst, scaler_interp_phases_neon(a, b, first));
        vst1q_u8(dst + 12, scaler_interp_phases_neon(a, b, second));
        vst1_u8(dst + 24, vget_low_u8(tail));
        memcpy(dst + 32, &last_word, sizeof(last_word));
    }
#endif
    for (; sx < groups; sx++) {
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
    (void)scratch;
#endif
}

static int scaler_scale_into(scaler *s, const djvu_cpix *in,
                             uint8_t *dst0, int stride, int topdown)
{
    djvu_ctx *ctx = s->ctx;
    int bufw, x, y, grouped, rc = -1, l1 = -1, l2 = -1;
    size_t ob;
    uint8_t *lbuf = NULL, *scratch = NULL, *p1 = NULL, *p2 = NULL;
    uint8_t *ga = NULL, *gb = NULL, *hph = NULL;
    uint32_t *hoff = NULL;

    if (!in || !in->d || in->w <= 0 || in->h <= 0 ||
        in->w != s->inw || in->h != s->inh)
        return -1;
    prepare_interp();
    if (!s->hcoord || !s->vcoord) return -1;
    if (s->xshift == 0 && s->yshift == 0 &&
        s->hnum == 3 && s->hden == 1 && s->vnum == 3 && s->vden == 1 &&
        s->inw > 0 && s->inh > 0 &&
        s->redw == s->inw && s->redh == s->inh &&
        s->outw >= 3 * s->inw - 2 && s->outw <= 3 * s->inw &&
        s->outh >= 3 * s->inh - 2 && s->outh <= 3 * s->inh &&
        in->w == s->inw && in->h == s->inh)
        return scaler_scale_red3_into(s, in, dst0, stride, topdown);

    /* Per output row: lbuf = vertical lerp of the two source rows (padded by
       one pixel each side), then the horizontal pass. Exact 4x / 12x
       upscales use the periodic phase expansion; every other ratio gathers
       each output pixel's two neighbours from lbuf and blends them in one
       SIMD pass with a per-byte phase. */
    grouped = s->xshift == 0 && s->hden == 1 && (s->hnum == 4 || s->hnum == 12);
    bufw = s->redw;
    ob = (size_t)s->outw * 3;
    lbuf = (uint8_t *)djvu_alloc(ctx, (size_t)(bufw + 2) * 3 + 4);
    if (!lbuf) goto done;
    if (grouped) {
        scratch = (uint8_t *)djvu_alloc(ctx, expand_scratch_size(bufw, s->hnum));
        if (!scratch) goto done;
    } else {
        hoff = (uint32_t *)djvu_alloc(ctx, sizeof(uint32_t) * (size_t)s->outw);
        hph = (uint8_t *)djvu_alloc(ctx, ob + 16);
        ga = (uint8_t *)djvu_alloc(ctx, ob + 4);
        gb = (uint8_t *)djvu_alloc(ctx, ob + 4);
        if (!hoff || !hph || !ga || !gb) goto done;
        for (x = 0; x < s->outw; x++) {
            int n = s->hcoord[x];
            /* lbuf pixel 1 + (n >> 4) and the next one; n >> 4 >= -1 */
            hoff[x] = (uint32_t)((1 + (n >> FRACBITS)) * 3);
            hph[x * 3 + 0] = hph[x * 3 + 1] = hph[x * 3 + 2] = (uint8_t)(n & FRACMASK);
        }
    }
    if (s->xshift > 0 || s->yshift > 0) {
        p1 = (uint8_t *)djvu_alloc(ctx, (size_t)bufw * 3);
        p2 = (uint8_t *)djvu_alloc(ctx, (size_t)bufw * 3);
        if (!p1 || !p2) goto done;
    }

    for (y = 0; y < s->outh; y++) {
        int fy, fy1, fy2;
        const uint8_t *lower, *upper;
        uint8_t *dest;

        /* abort checkpoint: scaling a cached background is the bulk of a
           cached photo-page render */
        if ((y & 63) == 0 && djvu_aborted(ctx)) goto done;
        fy = s->vcoord[y];
        fy1 = fy >> FRACBITS;
        fy2 = fy1 + 1;
        if (s->xshift > 0 || s->yshift > 0) {
            int want1 = fy1 < 0 ? 0 : (fy1 >= s->redh ? s->redh - 1 : fy1);
            int want2 = fy2 < 0 ? 0 : (fy2 >= s->redh ? s->redh - 1 : fy2);
            if (want1 == l2) lower = p2; else if (want1 == l1) lower = p1;
            else { uint8_t *t = p1; p1 = p2; l1 = l2; p2 = t; l2 = want1;
                   scaler_get_line(s, want1, in, 0, 0, 0, s->redw, p2); lower = p2; }
            if (want2 == l2) upper = p2; else if (want2 == l1) upper = p1;
            else { uint8_t *t = p1; p1 = p2; l1 = l2; p2 = t; l2 = want2;
                   scaler_get_line(s, want2, in, 0, 0, 0, s->redw, p2); upper = p2; }
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
        /* vertical lerp of the two source rows (same arithmetic, SIMD) */
        scaler_interp_row(lower, upper, bufw, fy & FRACMASK, lbuf + 3);
        lbuf[0] = lbuf[3]; lbuf[1] = lbuf[4]; lbuf[2] = lbuf[5];
        memcpy(lbuf + (size_t)(bufw + 1) * 3, lbuf + (size_t)bufw * 3, 3);
        lbuf[(size_t)(bufw + 2) * 3] = 0;    /* read (not kept) by the 4-byte copies */
        dest = scaler_dest_row(dst0, stride, s->outh, topdown, y);
        if (grouped) {
            if (s->hnum == 12)
                scaler_expand_row12(lbuf + 3, bufw, s->outw, dest, scratch);
            else
                scaler_expand_row4(lbuf + 3, bufw, s->outw, dest, scratch);
        } else {
            for (x = 0; x < s->outw; x++) {     /* 4-byte copies, 3 kept */
                memcpy(ga + (size_t)x * 3, lbuf + hoff[x], 4);
                memcpy(gb + (size_t)x * 3, lbuf + hoff[x] + 3, 4);
            }
            scaler_lerp_bytes(ga, gb, hph, ob, dest);
        }
    }
    rc = 0;
done:
    djvu_free(ctx, lbuf);
    djvu_free(ctx, scratch);
    djvu_free(ctx, p1);
    djvu_free(ctx, p2);
    djvu_free(ctx, hoff);
    djvu_free(ctx, hph);
    djvu_free(ctx, ga);
    djvu_free(ctx, gb);
    return rc;
}

static int scaler_scale(scaler *s, const djvu_cpix *in, djvu_cpix *out)
{
    if (djvu_cpix_init(s->ctx, out, s->outw, s->outh) != 0) return -1;
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
    return p->d ? 0 : -1;
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

/* Scale by an arbitrary rational factor: output = input * numer/denom.
   A reduced IW44 layer (reduction `red` vs the page) can be scaled straight to a
   subsampled target with numer=red, denom=subsample (denom=1 is the plain
   integer upscale by `red`) -- including fractional
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
    if (scaler_set_h(&s, numer, denom) != 0 ||
        scaler_set_v(&s, numer, denom) != 0 ||
        scaler_scale(&s, in, out) != 0) {
        /* never leave a half-written pixmap behind: the page cache treats a
           non-NULL bg_scaled as complete */
        djvu_cpix_free(ctx, out);
        scaler_free(&s);
        return -1;
    }
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
    if (scaler_set_h(&s, red, 1) != 0 || scaler_set_v(&s, red, 1) != 0 ||
        scaler_scale_into(&s, in, dst, stride, 1) != 0) {
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
