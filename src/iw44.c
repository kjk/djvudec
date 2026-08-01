/* iw44.c -- IW44 wavelet decoder. Ported from DjvuNet Wavelet/
 * {InterWaveCodec,InterWaveDecoder,InterWaveMap,InterWaveBlock,
 *  InterWavePixelMap,InterWavePixelMapDecoder,InterWaveTransform}.cs */
#include "djvu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SSE2 is baseline on x86-64; MSVC does not define __SSE2__ automatically.
   On Apple Silicon / aarch64 we use NEON for the same hot paths. */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define DJVU_IW44_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#define DJVU_IW44_NEON 1
#include <arm_neon.h>
#endif

extern const int16_t djvu_iw44_zigzag[1024];

/* ---------- block: 64 sparse buckets of 16 coefficients ---------- */

typedef struct {
    int16_t *buckets[64];   /* NULL or array of 16 */
} iw_block;

/* Slab of 16-coeff buckets: one malloc instead of one per sparse bucket.
   Photo pages allocate tens of thousands of 32-byte buckets; a slab of 256
   cuts allocator traffic and keeps coeffs denser in L1/L2. */
#define IW_BUCKET_SLAB 256

typedef struct iw_bucket_slab {
    struct iw_bucket_slab *next;
    int16_t data[IW_BUCKET_SLAB * 16];
} iw_bucket_slab;

typedef struct {
    int w, h, bw, bh, nb;
    iw_block *blocks;
    iw_bucket_slab *slabs;
    int slab_used;          /* buckets taken from slabs->data head */
    int n_buckets;          /* total live buckets (for mem accounting) */
} iw_map;

/* band bucket table: {start, size} */
static const int band_start[10] = {0, 1, 2, 3, 4, 8, 12, 16, 32, 48};
static const int band_size[10]  = {1, 1, 1, 1, 4, 4, 4, 16, 16, 16};

static const int iwquant[16] = {
    0x10000, 0x20000, 0x20000, 0x40000, 0x40000, 0x40000, 0x80000,
    0x80000, 0x80000, 0x100000, 0x100000, 0x100000, 0x200000,
    0x100000, 0x100000, 0x200000
};

typedef struct {
    djvu_ctx *ctx;
    iw_map *map;
    int8_t coeff_state[256];
    int8_t bucket_state[16];
    uint8_t ctx_start[32];
    uint8_t ctx_bucket[10][8];
    uint8_t ctx_mant, ctx_root;
    int quant_high[10];
    int quant_low[16];
    int curband, curbit;
} iw_codec;

struct iw_pixmap {
    djvu_ctx *ctx;
    /* Page-cache pin count (create at 1; retain on acquire; free unrefs). */
    djvu_refcount refs;
    iw_map *ymap, *cbmap, *crmap;
    iw_codec *yc, *cbc, *crc;
    int cslices, cserial;
    int crcbdelay, crcbhalf;
    int w, h;
};

/* ---------- map / block ---------- */

static iw_map *map_new(djvu_ctx *ctx, int w, int h)
{
    iw_map *m = (iw_map *)djvu_alloc(ctx, sizeof(iw_map));
    size_t blocks_bytes;
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->w = w; m->h = h;
    m->bw = (w + 0x20 - 1) & ~0x1f;
    m->bh = (h + 0x20 - 1) & ~0x1f;
    m->nb = (m->bw * m->bh) / 1024;
    blocks_bytes = sizeof(iw_block) * (size_t)m->nb;
    m->blocks = (iw_block *)djvu_alloc(ctx, blocks_bytes);
    if (!m->blocks) { djvu_free(ctx, m); return NULL; }
    memset(m->blocks, 0, blocks_bytes);
    return m;
}

static void map_free(djvu_ctx *ctx, iw_map *m)
{
    iw_bucket_slab *s;
    if (!m) return;
    /* Buckets live in slabs; free slabs only (not each pointer). */
    s = m->slabs;
    while (s) {
        iw_bucket_slab *n = s->next;
        djvu_free(ctx, s);
        s = n;
    }
    djvu_free(ctx, m->blocks);
    djvu_free(ctx, m);
}

static int16_t *map_alloc_bucket(djvu_ctx *ctx, iw_map *map)
{
    int16_t *p;
    if (!map->slabs || map->slab_used >= IW_BUCKET_SLAB) {
        iw_bucket_slab *s = (iw_bucket_slab *)djvu_alloc(ctx, sizeof(iw_bucket_slab));
        if (!s) return NULL;
        /* Zero whole slab so new buckets start clean without per-bucket memset. */
        memset(s->data, 0, sizeof(s->data));
        s->next = map->slabs;
        map->slabs = s;
        map->slab_used = 0;
    }
    p = map->slabs->data + (size_t)map->slab_used * 16;
    map->slab_used++;
    map->n_buckets++;
    return p;
}

static int16_t *block_get_init(djvu_ctx *ctx, iw_map *map, iw_block *blk, int n)
{
    if (!blk->buckets[n]) {
        blk->buckets[n] = map_alloc_bucket(ctx, map);
    }
    return blk->buckets[n];
}

/* Scatter sparse 16-coeff buckets into a already-zeroed 32×32 region of the
   unified transform buffer. Zigzag index → row-major (zi>>5)*bw + (zi&31).
   Avoids a 2 KB temp zero + full 32×32 memcpy per block (photo pages have
   thousands of blocks × 3 planes). */
static void scatter_lift_block(iw_block *blk, int16_t *data16, int base, int bw)
{
    int n = 0, n1, n2;
    for (n1 = 0; n1 < 64; n1++) {
        int16_t *d = blk->buckets[n1];
        if (d) {
            for (n2 = 0; n2 < 16; n2++, n++) {
                int zi = djvu_iw44_zigzag[n];
                data16[base + (zi >> 5) * bw + (zi & 31)] = d[n2];
            }
        } else {
            n += 16;
        }
    }
}

/* ---------- inverse wavelet transform (ported from DjVuLibre IW44Image.cpp
 * filter_bv / filter_bh -- the canonical reference) ---------- */

#ifdef DJVU_IW44_SSE2
/* Sign-extend 8×int16 to two 4×int32 vectors. */
static void bv_i16x8_to_i32(const int16_t *src, __m128i *lo, __m128i *hi)
{
    __m128i v = _mm_loadu_si128((const __m128i *)src);
    __m128i sign = _mm_cmpgt_epi16(_mm_setzero_si128(), v);
    *lo = _mm_unpacklo_epi16(v, sign);
    *hi = _mm_unpackhi_epi16(v, sign);
}

/* Truncating pack 8×int32 -> 8×int16 (matches C cast (int16_t), not sat). */
static __m128i bv_pack_i32_trunc(__m128i lo, __m128i hi)
{
    lo = _mm_srai_epi32(_mm_slli_epi32(lo, 16), 16);
    hi = _mm_srai_epi32(_mm_slli_epi32(hi, 16), 16);
    return _mm_packs_epi32(lo, hi);
}

/* Apply lift (sub=1) or interp (sub=0) for 8 contiguous samples at q+i. */
static void filter_bv_apply8_s1(int16_t *q, int i, int s, int s3, int lift)
{
    __m128i q_lo, q_hi, a_lo, a_hi, b_lo, b_hi, t_lo, t_hi;
    __m128i ms_lo, ms_hi, ps_lo, ps_hi, ms3_lo, ms3_hi, ps3_lo, ps3_hi;
    const __m128i bias = _mm_set1_epi32(lift ? 16 : 8);
    const int rshift = lift ? 5 : 4;
    bv_i16x8_to_i32(q + i, &q_lo, &q_hi);
    bv_i16x8_to_i32(q + i - s, &ms_lo, &ms_hi);
    bv_i16x8_to_i32(q + i + s, &ps_lo, &ps_hi);
    bv_i16x8_to_i32(q + i - s3, &ms3_lo, &ms3_hi);
    bv_i16x8_to_i32(q + i + s3, &ps3_lo, &ps3_hi);
    a_lo = _mm_add_epi32(ms_lo, ps_lo);
    a_hi = _mm_add_epi32(ms_hi, ps_hi);
    b_lo = _mm_add_epi32(ms3_lo, ps3_lo);
    b_hi = _mm_add_epi32(ms3_hi, ps3_hi);
    /* 9*a - b + bias, then >> rshift */
    t_lo = _mm_add_epi32(_mm_slli_epi32(a_lo, 3), a_lo);
    t_hi = _mm_add_epi32(_mm_slli_epi32(a_hi, 3), a_hi);
    t_lo = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(t_lo, b_lo), bias), rshift);
    t_hi = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(t_hi, b_hi), bias), rshift);
    if (lift) {
        q_lo = _mm_sub_epi32(q_lo, t_lo);
        q_hi = _mm_sub_epi32(q_hi, t_hi);
    } else {
        q_lo = _mm_add_epi32(q_lo, t_lo);
        q_hi = _mm_add_epi32(q_hi, t_hi);
    }
    _mm_storeu_si128((__m128i *)(q + i), bv_pack_i32_trunc(q_lo, q_hi));
}

/* Lift: *q -= ((9*a - b + 16) >> 5). Contiguous scale==1 interior. */
static void filter_bv_lift_interior_s1(int16_t *q, int w, int s, int s3)
{
    int i = 0;
    for (; i + 16 <= w; i += 16) {
        filter_bv_apply8_s1(q, i, s, s3, 1);
        filter_bv_apply8_s1(q, i + 8, s, s3, 1);
    }
    for (; i + 8 <= w; i += 8)
        filter_bv_apply8_s1(q, i, s, s3, 1);
    for (; i < w; i++) {
        int a = (int)q[i - s] + (int)q[i + s];
        int b = (int)q[i - s3] + (int)q[i + s3];
        q[i] = (int16_t)(q[i] - (((a << 3) + a - b + 16) >> 5));
    }
}

/* Interp: *q += ((9*a - b + 8) >> 4). Contiguous scale==1 interior. */
static void filter_bv_interp_interior_s1(int16_t *q, int w, int s, int s3)
{
    int i = 0;
    for (; i + 16 <= w; i += 16) {
        filter_bv_apply8_s1(q, i, s, s3, 0);
        filter_bv_apply8_s1(q, i + 8, s, s3, 0);
    }
    for (; i + 8 <= w; i += 8)
        filter_bv_apply8_s1(q, i, s, s3, 0);
    for (; i < w; i++) {
        int a = (int)q[i - s] + (int)q[i + s];
        int b = (int)q[i - s3] + (int)q[i + s3];
        q[i] = (int16_t)(q[i] + (((a << 3) + a - b + 8) >> 4));
    }
}
#endif /* DJVU_IW44_SSE2 */

#ifdef DJVU_IW44_NEON
/* Apply lift (sub=1) or interp (sub=0) for 8 contiguous samples at q+i.
   Truncating int32→int16 via vmovn matches C cast (int16_t), not sat. */
static void filter_bv_apply8_s1(int16_t *q, int i, int s, int s3, int lift)
{
    int16x8_t qv = vld1q_s16(q + i);
    int16x8_t ms = vld1q_s16(q + i - s);
    int16x8_t ps = vld1q_s16(q + i + s);
    int16x8_t ms3 = vld1q_s16(q + i - s3);
    int16x8_t ps3 = vld1q_s16(q + i + s3);
    int32x4_t a_lo = vaddl_s16(vget_low_s16(ms), vget_low_s16(ps));
    int32x4_t a_hi = vaddl_s16(vget_high_s16(ms), vget_high_s16(ps));
    int32x4_t b_lo = vaddl_s16(vget_low_s16(ms3), vget_low_s16(ps3));
    int32x4_t b_hi = vaddl_s16(vget_high_s16(ms3), vget_high_s16(ps3));
    /* 9*a - b + bias, then >> rshift */
    int32x4_t t_lo = vaddq_s32(vshlq_n_s32(a_lo, 3), a_lo);
    int32x4_t t_hi = vaddq_s32(vshlq_n_s32(a_hi, 3), a_hi);
    t_lo = vsubq_s32(t_lo, b_lo);
    t_hi = vsubq_s32(t_hi, b_hi);
    if (lift) {
        t_lo = vshrq_n_s32(vaddq_s32(t_lo, vdupq_n_s32(16)), 5);
        t_hi = vshrq_n_s32(vaddq_s32(t_hi, vdupq_n_s32(16)), 5);
        t_lo = vsubq_s32(vmovl_s16(vget_low_s16(qv)), t_lo);
        t_hi = vsubq_s32(vmovl_s16(vget_high_s16(qv)), t_hi);
    } else {
        t_lo = vshrq_n_s32(vaddq_s32(t_lo, vdupq_n_s32(8)), 4);
        t_hi = vshrq_n_s32(vaddq_s32(t_hi, vdupq_n_s32(8)), 4);
        t_lo = vaddq_s32(vmovl_s16(vget_low_s16(qv)), t_lo);
        t_hi = vaddq_s32(vmovl_s16(vget_high_s16(qv)), t_hi);
    }
    vst1q_s16(q + i, vcombine_s16(vmovn_s32(t_lo), vmovn_s32(t_hi)));
}

static void filter_bv_lift_interior_s1(int16_t *q, int w, int s, int s3)
{
    int i = 0;
    for (; i + 16 <= w; i += 16) {
        filter_bv_apply8_s1(q, i, s, s3, 1);
        filter_bv_apply8_s1(q, i + 8, s, s3, 1);
    }
    for (; i + 8 <= w; i += 8)
        filter_bv_apply8_s1(q, i, s, s3, 1);
    for (; i < w; i++) {
        int a = (int)q[i - s] + (int)q[i + s];
        int b = (int)q[i - s3] + (int)q[i + s3];
        q[i] = (int16_t)(q[i] - (((a << 3) + a - b + 16) >> 5));
    }
}

static void filter_bv_interp_interior_s1(int16_t *q, int w, int s, int s3)
{
    int i = 0;
    for (; i + 16 <= w; i += 16) {
        filter_bv_apply8_s1(q, i, s, s3, 0);
        filter_bv_apply8_s1(q, i + 8, s, s3, 0);
    }
    for (; i + 8 <= w; i += 8)
        filter_bv_apply8_s1(q, i, s, s3, 0);
    for (; i < w; i++) {
        int a = (int)q[i - s] + (int)q[i + s];
        int b = (int)q[i - s3] + (int)q[i + s3];
        q[i] = (int16_t)(q[i] + (((a << 3) + a - b + 8) >> 4));
    }
}
#endif /* DJVU_IW44_NEON */

static void filter_bv(int16_t *p, int w, int h, int rowsize, int scale)
{
    int y = 0;
    int s = scale * rowsize;
    int s3 = s + s + s;
    h = ((h - 1) / scale) + 1;
    while (y - 3 < h) {
        /* 1-Lifting */
        {
            int16_t *q = p;
            int16_t *e = q + w;
            if (y >= 3 && y + 3 < h) {
#if defined(DJVU_IW44_SSE2) || defined(DJVU_IW44_NEON)
                if (scale == 1) {
                    filter_bv_lift_interior_s1(q, w, s, s3);
                } else
#endif
                {
                    while (q < e) {
                        int a = (int)q[-s] + (int)q[s];
                        int b = (int)q[-s3] + (int)q[s3];
                        *q = (int16_t)(*q - (((a << 3) + a - b + 16) >> 5));
                        q += scale;
                    }
                }
            } else if (y < h) {
                int16_t *q1 = (y + 1 < h) ? q + s : NULL;
                int16_t *q3 = (y + 3 < h) ? q + s3 : NULL;
                if (y >= 3) {
                    while (q < e) {
                        int a = (int)q[-s] + (q1 ? (int)*q1 : 0);
                        int b = (int)q[-s3] + (q3 ? (int)*q3 : 0);
                        *q = (int16_t)(*q - (((a << 3) + a - b + 16) >> 5));
                        q += scale; if (q1) q1 += scale; if (q3) q3 += scale;
                    }
                } else if (y >= 1) {
                    while (q < e) {
                        int a = (int)q[-s] + (q1 ? (int)*q1 : 0);
                        int b = (q3 ? (int)*q3 : 0);
                        *q = (int16_t)(*q - (((a << 3) + a - b + 16) >> 5));
                        q += scale; if (q1) q1 += scale; if (q3) q3 += scale;
                    }
                } else {
                    while (q < e) {
                        int a = (q1 ? (int)*q1 : 0);
                        int b = (q3 ? (int)*q3 : 0);
                        *q = (int16_t)(*q - (((a << 3) + a - b + 16) >> 5));
                        q += scale; if (q1) q1 += scale; if (q3) q3 += scale;
                    }
                }
            }
        }
        /* 2-Interpolation */
        {
            int16_t *q = p - s3;
            int16_t *e = q + w;
            if (y >= 6 && y < h) {
#if defined(DJVU_IW44_SSE2) || defined(DJVU_IW44_NEON)
                if (scale == 1) {
                    filter_bv_interp_interior_s1(q, w, s, s3);
                } else
#endif
                {
                    while (q < e) {
                        int a = (int)q[-s] + (int)q[s];
                        int b = (int)q[-s3] + (int)q[s3];
                        *q = (int16_t)(*q + (((a << 3) + a - b + 8) >> 4));
                        q += scale;
                    }
                }
            } else if (y >= 3) {
                int16_t *q1 = (y - 2 < h) ? q + s : q - s;
                while (q < e) {
                    int a = (int)q[-s] + (int)*q1;
                    *q = (int16_t)(*q + ((a + 1) >> 1));
                    q += scale; q1 += scale;
                }
            }
        }
        y += 2;
        p += s + s;
    }
}

/* One horizontal filter pass over a single row (recurrence left→right).
   Rows are independent, so the dual-row path below interleaves two of these
   for ILP / dual memory streams (scale==1 interior). */
static void filter_bh_row(int16_t *p, int w, int s, int s3)
{
    int16_t *q = p;
    int16_t *e = p + w;
    int a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (q < e) {                         /* x = 0 */
        if (q + s < e) a2 = q[s];
        if (q + s3 < e) a3 = q[s3];
        b2 = b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q += s + s;
    }
    if (q < e) {                         /* x = 2 */
        a0 = a1; a1 = a2; a2 = a3;
        if (q + s3 < e) a3 = q[s3];
        b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q += s + s;
    }
    if (q < e) {                         /* x = 4 */
        b1 = b2; b2 = b3; a0 = a1; a1 = a2; a2 = a3;
        if (q + s3 < e) a3 = q[s3];
        b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q[-s3] = (int16_t)(q[-s3] + ((b1 + b2 + 1) >> 1));
        q += s + s;
    }
    while (q + s3 < e) {                  /* generic */
        a0 = a1; a1 = a2; a2 = a3; a3 = q[s3];
        b0 = b1; b1 = b2; b2 = b3;
        b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q[-s3] = (int16_t)(q[-s3] + ((((b1 + b2) << 3) + (b1 + b2) - b0 - b3 + 8) >> 4));
        q += s + s;
    }
    while (q < e) {                       /* w-3 <= x < w */
        a0 = a1; a1 = a2; a2 = a3; a3 = 0;
        b0 = b1; b1 = b2; b2 = b3;
        b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
        q[0] = (int16_t)b3;
        q[-s3] = (int16_t)(q[-s3] + ((((b1 + b2) << 3) + (b1 + b2) - b0 - b3 + 8) >> 4));
        q += s + s;
    }
    while (q - s3 < e) {                   /* w <= x < w+3 */
        b0 = b1; b1 = b2; b2 = b3;
        if (q - s3 >= p)
            q[-s3] = (int16_t)(q[-s3] + ((b1 + b2 + 1) >> 1));
        q += s + s;
    }
    (void)b0;
}

/* Two independent scale==1 rows interleaved for ILP. Same math as
   filter_bh_row × 2; only the generic / tail loops are fused. */
static void filter_bh_two_rows_s1(int16_t *p0, int16_t *p1, int w)
{
    const int s = 1, s3 = 3;
    int16_t *q0 = p0, *q1 = p1;
    int16_t *e0 = p0 + w;
    int a0_0 = 0, a1_0 = 0, a2_0 = 0, a3_0 = 0;
    int b0_0 = 0, b1_0 = 0, b2_0 = 0, b3_0 = 0;
    int a0_1 = 0, a1_1 = 0, a2_1 = 0, a3_1 = 0;
    int b0_1 = 0, b1_1 = 0, b2_1 = 0, b3_1 = 0;

    /* x = 0 */
    if (q0 < e0) {
        if (q0 + s < e0) { a2_0 = q0[s]; a2_1 = q1[s]; }
        if (q0 + s3 < e0) { a3_0 = q0[s3]; a3_1 = q1[s3]; }
        b2_0 = b3_0 = q0[0] - ((((a1_0 + a2_0) << 3) + (a1_0 + a2_0) - a0_0 - a3_0 + 16) >> 5);
        b2_1 = b3_1 = q1[0] - ((((a1_1 + a2_1) << 3) + (a1_1 + a2_1) - a0_1 - a3_1 + 16) >> 5);
        q0[0] = (int16_t)b3_0; q1[0] = (int16_t)b3_1;
        q0 += 2; q1 += 2;
    }
    /* x = 2 */
    if (q0 < e0) {
        a0_0 = a1_0; a1_0 = a2_0; a2_0 = a3_0;
        a0_1 = a1_1; a1_1 = a2_1; a2_1 = a3_1;
        if (q0 + s3 < e0) { a3_0 = q0[s3]; a3_1 = q1[s3]; }
        b3_0 = q0[0] - ((((a1_0 + a2_0) << 3) + (a1_0 + a2_0) - a0_0 - a3_0 + 16) >> 5);
        b3_1 = q1[0] - ((((a1_1 + a2_1) << 3) + (a1_1 + a2_1) - a0_1 - a3_1 + 16) >> 5);
        q0[0] = (int16_t)b3_0; q1[0] = (int16_t)b3_1;
        q0 += 2; q1 += 2;
    }
    /* x = 4 */
    if (q0 < e0) {
        b1_0 = b2_0; b2_0 = b3_0; a0_0 = a1_0; a1_0 = a2_0; a2_0 = a3_0;
        b1_1 = b2_1; b2_1 = b3_1; a0_1 = a1_1; a1_1 = a2_1; a2_1 = a3_1;
        if (q0 + s3 < e0) { a3_0 = q0[s3]; a3_1 = q1[s3]; }
        b3_0 = q0[0] - ((((a1_0 + a2_0) << 3) + (a1_0 + a2_0) - a0_0 - a3_0 + 16) >> 5);
        b3_1 = q1[0] - ((((a1_1 + a2_1) << 3) + (a1_1 + a2_1) - a0_1 - a3_1 + 16) >> 5);
        q0[0] = (int16_t)b3_0; q1[0] = (int16_t)b3_1;
        q0[-s3] = (int16_t)(q0[-s3] + ((b1_0 + b2_0 + 1) >> 1));
        q1[-s3] = (int16_t)(q1[-s3] + ((b1_1 + b2_1 + 1) >> 1));
        q0 += 2; q1 += 2;
    }
    while (q0 + s3 < e0) {
        a0_0 = a1_0; a1_0 = a2_0; a2_0 = a3_0; a3_0 = q0[s3];
        a0_1 = a1_1; a1_1 = a2_1; a2_1 = a3_1; a3_1 = q1[s3];
        b0_0 = b1_0; b1_0 = b2_0; b2_0 = b3_0;
        b0_1 = b1_1; b1_1 = b2_1; b2_1 = b3_1;
        b3_0 = q0[0] - ((((a1_0 + a2_0) << 3) + (a1_0 + a2_0) - a0_0 - a3_0 + 16) >> 5);
        b3_1 = q1[0] - ((((a1_1 + a2_1) << 3) + (a1_1 + a2_1) - a0_1 - a3_1 + 16) >> 5);
        q0[0] = (int16_t)b3_0; q1[0] = (int16_t)b3_1;
        q0[-s3] = (int16_t)(q0[-s3] + ((((b1_0 + b2_0) << 3) + (b1_0 + b2_0) - b0_0 - b3_0 + 8) >> 4));
        q1[-s3] = (int16_t)(q1[-s3] + ((((b1_1 + b2_1) << 3) + (b1_1 + b2_1) - b0_1 - b3_1 + 8) >> 4));
        q0 += 2; q1 += 2;
    }
    while (q0 < e0) {
        a0_0 = a1_0; a1_0 = a2_0; a2_0 = a3_0; a3_0 = 0;
        a0_1 = a1_1; a1_1 = a2_1; a2_1 = a3_1; a3_1 = 0;
        b0_0 = b1_0; b1_0 = b2_0; b2_0 = b3_0;
        b0_1 = b1_1; b1_1 = b2_1; b2_1 = b3_1;
        b3_0 = q0[0] - ((((a1_0 + a2_0) << 3) + (a1_0 + a2_0) - a0_0 - a3_0 + 16) >> 5);
        b3_1 = q1[0] - ((((a1_1 + a2_1) << 3) + (a1_1 + a2_1) - a0_1 - a3_1 + 16) >> 5);
        q0[0] = (int16_t)b3_0; q1[0] = (int16_t)b3_1;
        q0[-s3] = (int16_t)(q0[-s3] + ((((b1_0 + b2_0) << 3) + (b1_0 + b2_0) - b0_0 - b3_0 + 8) >> 4));
        q1[-s3] = (int16_t)(q1[-s3] + ((((b1_1 + b2_1) << 3) + (b1_1 + b2_1) - b0_1 - b3_1 + 8) >> 4));
        q0 += 2; q1 += 2;
    }
    while (q0 - s3 < e0) {
        b0_0 = b1_0; b1_0 = b2_0; b2_0 = b3_0;
        b0_1 = b1_1; b1_1 = b2_1; b2_1 = b3_1;
        if (q0 - s3 >= p0)
            q0[-s3] = (int16_t)(q0[-s3] + ((b1_0 + b2_0 + 1) >> 1));
        if (q1 - s3 >= p1)
            q1[-s3] = (int16_t)(q1[-s3] + ((b1_1 + b2_1 + 1) >> 1));
        q0 += 2; q1 += 2;
    }
    (void)b0_0; (void)b0_1;
}

static void filter_bh(int16_t *p, int w, int h, int rowsize, int scale)
{
    int y = 0;
    int s = scale;
    int s3 = s + s + s;
    int step = scale * rowsize; /* advance in int16 samples */
    if (scale == 1) {
        /* Dual-row ILP path; leftover single row uses the same math. */
        while (y + 1 < h) {
            filter_bh_two_rows_s1(p, p + rowsize, w);
            y += 2;
            p += 2 * rowsize;
        }
        if (y < h)
            filter_bh_row(p, w, s, s3);
        return;
    }
    while (y < h) {
        filter_bh_row(p, w, s, s3);
        y += scale;
        p += step;
    }
}

static void backward(int16_t *p, int w, int h, int rowsize, int begin, int end)
{
    int scale;
    for (scale = begin >> 1; scale >= end; scale >>= 1) {
        filter_bv(p, w, h, rowsize, scale);
        filter_bh(p, w, h, rowsize, scale);
    }
}

static int16_t *build_unified(djvu_ctx *ctx, iw_map *m)
{
    /* allocate with a safety margin (filter may look one macroblock ahead) */
    size_t n = (size_t)m->bw * m->bh + (size_t)m->bw * 4 + 16;
    int16_t *data16 = (int16_t *)djvu_alloc(ctx, sizeof(int16_t) * n);
    int blockidx = 0, i, j, pidx = 0;
    if (!data16) return NULL;
    /* Zero once; scatter_lift_block only writes live sparse buckets. */
    memset(data16, 0, sizeof(int16_t) * n);

    for (i = 0; i < m->bh; i += 32, pidx += 32 * m->bw) {
        for (j = 0; j < m->bw; j += 32) {
            scatter_lift_block(&m->blocks[blockidx], data16, pidx + j, m->bw);
            blockidx++;
        }
    }
    return data16;
}

/* Clamp (src[j]+32)>>6 to int8. Contiguous planar rows (pixsep=1) only. */
static void map_image_clamp_row(const int16_t *src, int8_t *dst, int w)
{
    int j = 0;
#ifdef DJVU_IW44_SSE2
    /* int16 math is not enough: scalar uses int (src+32)>>6. Widen to int32. */
    {
        const __m128i thirty_two = _mm_set1_epi32(32);
        for (; j + 8 <= w; j += 8) {
            __m128i v = _mm_loadu_si128((const __m128i *)(src + j));
            __m128i sign = _mm_cmpgt_epi16(_mm_setzero_si128(), v);
            __m128i lo = _mm_unpacklo_epi16(v, sign);
            __m128i hi = _mm_unpackhi_epi16(v, sign);
            lo = _mm_srai_epi32(_mm_add_epi32(lo, thirty_two), 6);
            hi = _mm_srai_epi32(_mm_add_epi32(hi, thirty_two), 6);
            /* packs_epi32 -> int16 sat; packs_epi16 -> int8 sat [-128,127] */
            {
                __m128i p16 = _mm_packs_epi32(lo, hi);
                __m128i p8 = _mm_packs_epi16(p16, p16);
                _mm_storel_epi64((__m128i *)(dst + j), p8);
            }
        }
    }
#endif
#ifdef DJVU_IW44_NEON
    {
        const int32x4_t thirty_two = vdupq_n_s32(32);
        for (; j + 16 <= w; j += 16) {
            int16x8_t v0 = vld1q_s16(src + j);
            int16x8_t v1 = vld1q_s16(src + j + 8);
            int32x4_t lo0 = vaddq_s32(vmovl_s16(vget_low_s16(v0)), thirty_two);
            int32x4_t hi0 = vaddq_s32(vmovl_s16(vget_high_s16(v0)), thirty_two);
            int32x4_t lo1 = vaddq_s32(vmovl_s16(vget_low_s16(v1)), thirty_two);
            int32x4_t hi1 = vaddq_s32(vmovl_s16(vget_high_s16(v1)), thirty_two);
            /* Arithmetic >> 6, then saturating narrow to int8 [-128,127]. */
            int16x8_t p0 = vcombine_s16(vqmovn_s32(vshrq_n_s32(lo0, 6)),
                                        vqmovn_s32(vshrq_n_s32(hi0, 6)));
            int16x8_t p1 = vcombine_s16(vqmovn_s32(vshrq_n_s32(lo1, 6)),
                                        vqmovn_s32(vshrq_n_s32(hi1, 6)));
            vst1q_s8(dst + j, vcombine_s8(vqmovn_s16(p0), vqmovn_s16(p1)));
        }
        for (; j + 8 <= w; j += 8) {
            int16x8_t v = vld1q_s16(src + j);
            int32x4_t lo = vaddq_s32(vmovl_s16(vget_low_s16(v)), thirty_two);
            int32x4_t hi = vaddq_s32(vmovl_s16(vget_high_s16(v)), thirty_two);
            int16x8_t p16 = vcombine_s16(vqmovn_s32(vshrq_n_s32(lo, 6)),
                                         vqmovn_s32(vshrq_n_s32(hi, 6)));
            vst1_s8(dst + j, vqmovn_s16(p16));
        }
    }
#endif
    for (; j < w; j++) {
        int x = ((int)src[j] + 32) >> 6;
        if (x < -128) x = -128;
        else if (x > 127) x = 127;
        dst[j] = (int8_t)x;
    }
}

/* produce signed 8-bit samples into img8 (stride rowsize, step pixsep). */
static int map_image(djvu_ctx *ctx, iw_map *m, int index, int8_t *img8,
                     int rowsize, int pixsep, int fast)
{
    int16_t *data16 = build_unified(ctx, m);
    int i, j, pidx, rowidx, pixidx;
    if (!data16) return -1;

    if (fast) {
        backward(data16, m->w, m->h, m->bw, 32, 2);
        pidx = 0;
        for (i = 0; i < m->bh; i += 2, pidx += m->bw) {
            for (j = 0; j < m->bw; j += 2, pidx += 2)
                data16[pidx + m->bw] = data16[pidx + m->bw + 1] =
                    data16[pidx + 1] = data16[pidx];
        }
    } else {
        backward(data16, m->w, m->h, m->bw, 32, 1);
    }

    pidx = 0;
    /* Planar contiguous rows: SIMD clamp; still used for debug gray/plane. */
    if (pixsep == 1 && rowsize == m->w) {
        for (i = 0, rowidx = index; i < m->h; i++, rowidx += rowsize, pidx += m->bw)
            map_image_clamp_row(data16 + pidx, img8 + rowidx, m->w);
    } else {
        for (i = 0, rowidx = index; i < m->h; i++, rowidx += rowsize, pidx += m->bw) {
            for (j = 0, pixidx = rowidx; j < m->w; j++, pixidx += pixsep) {
                int x = ((int)data16[pidx + j] + 32) >> 6;
                if (x < -128) x = -128;
                else if (x > 127) x = 127;
                img8[pixidx] = (int8_t)x;
            }
        }
    }
    djvu_free(ctx, data16);
    return 0;
}

/* ---------- codec ---------- */

static int next_quant(iw_codec *c)
{
    int flag = 0, i;
    for (i = 0; i < 16; i++)
        if ((c->quant_low[i] = c->quant_low[i] >> 1) != 0) flag = 1;
    for (i = 0; i < 10; i++)
        if ((c->quant_high[i] = c->quant_high[i] >> 1) != 0) flag = 1;
    return flag;
}

static void codec_init(iw_codec *c, djvu_ctx *ctx, iw_map *map)
{
    int i, j, qidx;
    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    c->map = map;
    c->curband = 0;
    c->curbit = 1;

    i = 0; qidx = 0;
    for (j = 0; i < 4; j++) c->quant_low[i++] = iwquant[qidx++];
    for (j = 0; j < 4; j++) c->quant_low[i++] = iwquant[qidx];
    qidx++;
    for (j = 0; j < 4; j++) c->quant_low[i++] = iwquant[qidx];
    qidx++;
    for (j = 0; j < 4; j++) c->quant_low[i++] = iwquant[qidx];
    qidx++;
    c->quant_high[0] = 0;
    for (j = 1; j < 10; j++) c->quant_high[j] = iwquant[qidx++];
    while (c->quant_low[0] >= 32768) next_quant(c);
}

static int is_null_slice(iw_codec *c, int bit, int band)
{
    int i, thr;
    (void)bit;
    if (band == 0) {
        int is_null = 1;
        for (i = 0; i < 16; i++) {
            int threshold = c->quant_low[i];
            c->coeff_state[i] = 1;
            if (threshold > 0 && threshold < 32768) {
                is_null = 0;
                c->coeff_state[i] = 0;
            }
        }
        return is_null;
    }
    thr = c->quant_high[band];
    if (thr <= 0 || thr >= 32768) return 1;
    for (i = 0; i < (band_size[band] << 4); i++)
        c->coeff_state[i] = 0;
    return 0;
}

/* IW44 keeps a/fence in locals across the dense coefficient loops. Keep the
   slow renormalization paths local too: round-tripping `a` through the large
   djvu_zp struct for every coded coefficient otherwise creates avoidable
   loads/stores in the hottest photo-decode loop. */
static inline void iw_zp_renorm_lps(djvu_zp *DJVU_RESTRICT zp,
                                    uint32_t *DJVU_RESTRICT a,
                                    uint32_t *DJVU_RESTRICT fence,
                                    uint32_t z)
{
    int shift;
    z = 0x10000u - z;
    *a += z;
    zp->code += z;
    shift = zp_ffz(zp, *a);
    zp->scount -= (uint8_t)shift;
    *a = (*a << shift) & 0xffff;
    zp->code = ((zp->code << shift) & 0xffff)
             | ((zp->buffer >> zp->scount) & ((1u << shift) - 1));
    if (zp->scount < 16)
        zp_preload(zp);
    *fence = zp->code < 0x8000 ? zp->code : 0x7fff;
}

static inline void iw_zp_renorm_mps(djvu_zp *DJVU_RESTRICT zp,
                                    uint32_t *DJVU_RESTRICT a,
                                    uint32_t *DJVU_RESTRICT fence,
                                    uint32_t z)
{
    zp->scount -= 1;
    *a = (z << 1) & 0xffff;
    zp->code = ((zp->code << 1) & 0xffff)
             | ((zp->buffer >> zp->scount) & 1);
    if (zp->scount < 16)
        zp_preload(zp);
    *fence = zp->code < 0x8000 ? zp->code : 0x7fff;
}

static inline int iw_zp_dec(djvu_zp *DJVU_RESTRICT zp,
                            uint32_t *DJVU_RESTRICT a,
                            uint32_t *DJVU_RESTRICT fence,
                            uint8_t *DJVU_RESTRICT ctx)
{
    uint32_t z = *a + zp->p[*ctx];
    int mps;
    if (DJVU_LIKELY(z <= *fence)) {
        *a = z;
        return *ctx & 1;
    }
    mps = *ctx & 1;
    {
        uint32_t d = 0x6000u + ((z + *a) >> 2);
        if (z > d) z = d;
    }
    if (z > zp->code) {
        *ctx = zp->dn[*ctx];
        iw_zp_renorm_lps(zp, a, fence, z);
        return mps ^ 1;
    }
    if (*a >= zp->m[*ctx])
        *ctx = zp->up[*ctx];
    iw_zp_renorm_mps(zp, a, fence, z);
    return mps;
}

static inline int iw_zp_dec_iw(djvu_zp *DJVU_RESTRICT zp,
                               uint32_t *DJVU_RESTRICT a,
                               uint32_t *DJVU_RESTRICT fence)
{
    uint32_t z = 0x8000u + ((*a + *a + *a) >> 3);
    if (z > zp->code) {
        iw_zp_renorm_lps(zp, a, fence, z);
        return 1;
    }
    iw_zp_renorm_mps(zp, a, fence, z);
    return 0;
}

static void decode_buckets(iw_codec *c, djvu_zp *zp, int bit, int band,
                           iw_block *blk, int fbucket, int nbucket)
{
    int thres = c->quant_high[band];
    int bbstate = 0;
    int8_t *DJVU_RESTRICT cstate = c->coeff_state;
    int8_t *DJVU_RESTRICT bstate = c->bucket_state;
    int16_t *pbuck[16];
    int cidx = 0, buckno, i;
    uint32_t a, fence;
    const int band0 = (band == 0);

    (void)bit;

    /* Cache bucket pointers once — every phase reuses them. */
    for (buckno = 0; buckno < nbucket; buckno++)
        pbuck[buckno] = blk->buckets[fbucket + buckno];

    for (buckno = 0; buckno < nbucket; buckno++, cidx += 16) {
        int bstatetmp = 0;
        int16_t *pcoeff = pbuck[buckno];
        if (pcoeff == NULL) {
            bstatetmp = 8;
        } else if (!band0) {
            /* is_null_slice clears every nonzero-band coefficient state.
               Rebuilding it therefore needs no preserve-ZERO test. */
            for (i = 0; i < 16; i++) {
                int cstatetmp = pcoeff[i] != 0 ? 2 : 8;
                cstate[cidx + i] = (int8_t)cstatetmp;
                bstatetmp |= cstatetmp;
            }
        } else {
            for (i = 0; i < 16; i++) {
                int cstatetmp = cstate[cidx + i] & 1;
                if (cstatetmp == 0)
                    cstatetmp |= (pcoeff[i] != 0) ? 2 : 8;
                cstate[cidx + i] = (int8_t)cstatetmp;
                bstatetmp |= cstatetmp;
            }
        }
        bstate[buckno] = (int8_t)bstatetmp;
        bbstate |= bstatetmp;
    }

    a = zp->a;
    fence = zp->fence;

    if (nbucket < 16 || (bbstate & 2) != 0) {
        bbstate |= 4;
    } else if ((bbstate & 8) != 0) {
        if (iw_zp_dec(zp, &a, &fence, &c->ctx_root) != 0)
            bbstate |= 4;
    }

    if ((bbstate & 4) != 0) {
        for (buckno = 0; buckno < nbucket; buckno++) {
            if ((bstate[buckno] & 8) != 0) {
                int ctx = 0;
                if (!band0) {
                    int k = (fbucket + buckno) << 2;
                    int16_t *b = blk->buckets[k >> 4];
                    if (b != NULL) {
                        k &= 0xf;
                        ctx = (b[k] != 0) + (b[k + 1] != 0)
                            + (b[k + 2] != 0) + (b[k + 3] != 0);
                        if (ctx > 3) ctx = 3;
                    }
                }
                if ((bbstate & 2) != 0) ctx |= 4;
                if (iw_zp_dec(zp, &a, &fence, &c->ctx_bucket[band][ctx]) != 0)
                    bstate[buckno] |= 4;
            }
        }
    }

    if ((bbstate & 4) != 0) {
        cidx = 0;
        for (buckno = 0; buckno < nbucket; buckno++, cidx += 16) {
            if ((bstate[buckno] & 4) != 0) {
                int16_t *pcoeff = pbuck[buckno];
                int gotcha = 0, maxgotcha = 7;
                if (pcoeff == NULL) {
                    pcoeff = block_get_init(c->ctx, c->map, blk, fbucket + buckno);
                    pbuck[buckno] = pcoeff;
                    if (!pcoeff) {
                        zp->a = a;
                        zp->fence = fence;
                        return;
                    }
                    for (i = 0; i < 16; i++)
                        if ((cstate[cidx + i] & 1) == 0) cstate[cidx + i] = 8;
                }
                for (i = 0; i < 16; i++)
                    if ((cstate[cidx + i] & 8) != 0) gotcha++;
                for (i = 0; i < 16; i++) {
                    if ((cstate[cidx + i] & 8) != 0) {
                        int ctx, coeff, halfthres;
                        int t = band0 ? c->quant_low[i] : thres;
                        ctx = (gotcha >= maxgotcha) ? maxgotcha : gotcha;
                        if ((bstate[buckno] & 2) != 0) ctx |= 8;
                        if (iw_zp_dec(zp, &a, &fence, &c->ctx_start[ctx]) != 0) {
                            cstate[cidx + i] |= 4;
                            halfthres = t >> 1;
                            coeff = (t + halfthres) - (halfthres >> 2);
                            {
                                int neg = -iw_zp_dec_iw(zp, &a, &fence);
                                pcoeff[i] = (int16_t)((coeff ^ neg) - neg);
                            }
                        }
                        if ((cstate[cidx + i] & 4) != 0) gotcha = 0;
                        else if (gotcha > 0) gotcha--;
                    }
                }
            }
        }
    }

    if ((bbstate & 2) != 0) {
        cidx = 0;
        for (buckno = 0; buckno < nbucket; buckno++, cidx += 16) {
            if ((bstate[buckno] & 2) != 0) {
                int16_t *pcoeff = pbuck[buckno];
                for (i = 0; i < 16; i++) {
                    if ((cstate[cidx + i] & 2) != 0) {
                        int coeff = pcoeff[i];
                        int t = band0 ? c->quant_low[i] : thres;
                        if (coeff < 0) coeff = -coeff;
                        if (coeff <= (3 * t)) {
                            coeff += (t >> 2);
                            if (iw_zp_dec(zp, &a, &fence, &c->ctx_mant) != 0)
                                coeff += (t >> 1);
                            else
                                coeff = (coeff - t) + (t >> 1);
                        } else {
                            if (iw_zp_dec_iw(zp, &a, &fence) != 0)
                                coeff += (t >> 1);
                            else
                                coeff = (coeff - t) + (t >> 1);
                        }
                        {
                            int neg = -(pcoeff[i] < 0);
                            pcoeff[i] = (int16_t)((coeff ^ neg) - neg);
                        }
                    }
                }
            }
        }
    }

    zp->a = a;
    zp->fence = fence;
}

/* decode one slice; returns 1 if more slices follow, 0 if done */
static int code_slice(iw_codec *c, djvu_zp *zp)
{
    if (c->curbit < 0) return 0;
    if (!is_null_slice(c, c->curbit, c->curband)) {
        int blockno;
        int fbucket = band_start[c->curband];
        int nbucket = band_size[c->curband];
        for (blockno = 0; blockno < c->map->nb; blockno++)
            decode_buckets(c, zp, c->curbit, c->curband,
                           &c->map->blocks[blockno], fbucket, nbucket);
    }
    if (++c->curband >= 10) {
        c->curband = 0;
        c->curbit++;
        if (next_quant(c) == 0) {
            c->curbit = -1;
            return 0;
        }
    }
    return 1;
}

/* ---------- pixmap ---------- */

iw_pixmap *djvu_iw44_new(djvu_ctx *ctx)
{
    iw_pixmap *pm = (iw_pixmap *)djvu_alloc(ctx, sizeof(iw_pixmap));
    if (!pm) return NULL;
    memset(pm, 0, sizeof(*pm));
    pm->ctx = ctx;
    djvu_refcount_init(&pm->refs, 1);
    pm->crcbdelay = 10;
    return pm;
}

void djvu_iw44_retain(iw_pixmap *pm)
{
    if (pm) djvu_refcount_retain(&pm->refs);
}

void djvu_iw44_free(iw_pixmap *pm)
{
    djvu_ctx *ctx;
    if (!pm) return;
    if (djvu_refcount_release(&pm->refs) > 0)
        return;
    ctx = pm->ctx;
    map_free(ctx, pm->ymap);
    map_free(ctx, pm->cbmap);
    map_free(ctx, pm->crmap);
    djvu_free(ctx, pm->yc);
    djvu_free(ctx, pm->cbc);
    djvu_free(ctx, pm->crc);
    djvu_free(ctx, pm);
}

static size_t map_mem_size(const iw_map *m)
{
    size_t n;
    int nslabs;
    if (!m) return 0;
    n = sizeof(iw_map);
    if (m->blocks && m->nb > 0)
        n += sizeof(iw_block) * (size_t)m->nb;
    /* Live buckets + slab overhead (allocated capacity, not just used). */
    nslabs = 0;
    {
        const iw_bucket_slab *s = m->slabs;
        while (s) { nslabs++; s = s->next; }
    }
    n += (size_t)nslabs * sizeof(iw_bucket_slab);
    return n;
}

size_t djvu_iw44_mem_size(const iw_pixmap *pm)
{
    size_t n;
    if (!pm) return 0;
    n = sizeof(iw_pixmap);
    n += map_mem_size(pm->ymap);
    n += map_mem_size(pm->cbmap);
    n += map_mem_size(pm->crmap);
    if (pm->yc) n += sizeof(iw_codec);
    if (pm->cbc) n += sizeof(iw_codec);
    if (pm->crc) n += sizeof(iw_codec);
    return n;
}

int djvu_iw44_decode_chunk(iw_pixmap *pm, const uint8_t *data, size_t len)
{
    djvu_ctx *ctx = pm->ctx;
    djvu_zp zp;
    size_t pos = 0;
    int serial, slices, nslices, flag;

    if (len < 2) return -1;
    serial = data[pos++];
    slices = data[pos++];
    if (serial != pm->cserial) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "IW44: out-of-order chunk");
        return -1;
    }
    nslices = pm->cslices + slices;

    if (pm->cserial == 0) {
        int major, minor, w, h, crcbdelay = 0;
        if (pos + 6 > len) return -1;
        major = data[pos++];
        minor = data[pos++];
        if ((major & 0x7f) != 1) {
            djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "IW44: incompatible codec");
            return -1;
        }
        w = (data[pos] << 8) | data[pos + 1]; pos += 2;
        h = (data[pos] << 8) | data[pos + 1]; pos += 2;
        if ((major & 0x7f) == 1 && minor >= 2) {
            if (pos >= len) return -1;
            crcbdelay = data[pos++];
            pm->crcbdelay = crcbdelay & 0x7f;
        }
        if (minor >= 2)
            pm->crcbhalf = (crcbdelay & 0x80) ? 0 : 1;
        if (major & 0x80)
            pm->crcbdelay = -1;

        pm->w = w; pm->h = h;
        pm->ymap = map_new(ctx, w, h);
        pm->yc = (iw_codec *)djvu_alloc(ctx, sizeof(iw_codec));
        if (!pm->ymap || !pm->yc) return -1;
        codec_init(pm->yc, ctx, pm->ymap);
        if (pm->crcbdelay >= 0) {
            pm->cbmap = map_new(ctx, w, h);
            pm->crmap = map_new(ctx, w, h);
            pm->cbc = (iw_codec *)djvu_alloc(ctx, sizeof(iw_codec));
            pm->crc = (iw_codec *)djvu_alloc(ctx, sizeof(iw_codec));
            if (!pm->cbmap || !pm->crmap || !pm->cbc || !pm->crc) return -1;
            codec_init(pm->cbc, ctx, pm->cbmap);
            codec_init(pm->crc, ctx, pm->crmap);
        }
    }

    if (getenv("DJVU_IW_DEBUG"))
        fprintf(stderr, "IW44 chunk: serial=%d slices=%d nslices=%d w=%d h=%d "
                "crcbdelay=%d crcbhalf=%d color=%d\n", serial, slices, nslices,
                pm->w, pm->h, pm->crcbdelay, pm->crcbhalf, djvu_iw44_is_color(pm));

    djvu_zp_init(&zp, data + pos, len - pos);

    for (flag = 1; flag != 0 && pm->cslices < nslices; pm->cslices++) {
        flag = code_slice(pm->yc, &zp);
        if (pm->crc && pm->cbc && pm->crcbdelay <= pm->cslices) {
            flag |= code_slice(pm->cbc, &zp);
            flag |= code_slice(pm->crc, &zp);
        }
    }
    pm->cserial++;
    return 0;
}

int djvu_iw44_decode_form(djvu_doc *doc, uint32_t form_off, const char *chunk_id,
                          iw_pixmap *pm, int max_chunks)
{
    uint32_t start = 0, sz;
    const uint8_t *chunk;
    int n = 0;

    while ((chunk = djvu_form_find_chunk(doc, form_off, chunk_id, &sz, &start)) != NULL) {
        if (max_chunks > 0 && n >= max_chunks) break;
        if (djvu_iw44_decode_chunk(pm, chunk, sz) != 0) return -1;
        n++;
    }
    return n > 0 ? 0 : -1;
}

int djvu_iw44_width(iw_pixmap *pm) { return pm ? pm->w : 0; }
int djvu_iw44_height(iw_pixmap *pm) { return pm ? pm->h : 0; }
int djvu_iw44_is_color(iw_pixmap *pm)
{
    return pm && pm->crmap && pm->cbmap && pm->crcbdelay >= 0;
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Convert one planar YCbCr row (int8 samples) to interleaved RGB24.
   y/b/r point at w samples; dst is w*3 bytes. Byte-exact vs scalar recipe. */
static void ycbcr_row_to_rgb(const int8_t *y, const int8_t *b, const int8_t *r,
                             uint8_t *dst, int w)
{
    int x = 0;
#ifdef DJVU_IW44_SSE2
    {
        const __m128i c128 = _mm_set1_epi16(128);
        for (; x + 8 <= w; x += 8) {
            __m128i y8 = _mm_loadl_epi64((const __m128i *)(y + x));
            __m128i b8 = _mm_loadl_epi64((const __m128i *)(b + x));
            __m128i r8 = _mm_loadl_epi64((const __m128i *)(r + x));
            /* sign-extend int8 -> int16 */
            __m128i yv = _mm_srai_epi16(_mm_unpacklo_epi8(y8, y8), 8);
            __m128i bv = _mm_srai_epi16(_mm_unpacklo_epi8(b8, b8), 8);
            __m128i rv = _mm_srai_epi16(_mm_unpacklo_epi8(r8, r8), 8);
            __m128i t1 = _mm_srai_epi16(bv, 2);
            __m128i t2 = _mm_add_epi16(rv, _mm_srai_epi16(rv, 1));
            __m128i y128 = _mm_add_epi16(yv, c128);
            __m128i t3 = _mm_sub_epi16(y128, t1);
            __m128i tr = _mm_add_epi16(y128, t2);
            __m128i tg = _mm_sub_epi16(t3, _mm_srai_epi16(t2, 1));
            __m128i tb = _mm_add_epi16(t3, _mm_slli_epi16(bv, 1));
            /* packus: signed int16 -> uint8 sat 0..255 (= clamp255) */
            __m128i ru = _mm_packus_epi16(tr, tr);
            __m128i gu = _mm_packus_epi16(tg, tg);
            __m128i bu = _mm_packus_epi16(tb, tb);
            /* Pack 8 RGB pixels: unpack to R G B 0 dwords, write 3 bytes each. */
            {
                __m128i rg = _mm_unpacklo_epi8(ru, gu);
                __m128i bz = _mm_unpacklo_epi8(bu, _mm_setzero_si128());
                __m128i pack0 = _mm_unpacklo_epi16(rg, bz); /* px 0-3: R G B 0 */
                __m128i pack1 = _mm_unpacklo_epi16(_mm_unpackhi_epi64(rg, rg),
                                                   _mm_unpackhi_epi64(bz, bz));
                uint32_t pix[4];
                int k;
                _mm_storeu_si128((__m128i *)pix, pack0);
                for (k = 0; k < 4; k++) {
                    uint32_t p = pix[k];
                    dst[0] = (uint8_t)p;
                    dst[1] = (uint8_t)(p >> 8);
                    dst[2] = (uint8_t)(p >> 16);
                    dst += 3;
                }
                _mm_storeu_si128((__m128i *)pix, pack1);
                for (k = 0; k < 4; k++) {
                    uint32_t p = pix[k];
                    dst[0] = (uint8_t)p;
                    dst[1] = (uint8_t)(p >> 8);
                    dst[2] = (uint8_t)(p >> 16);
                    dst += 3;
                }
            }
        }
    }
#endif
#ifdef DJVU_IW44_NEON
    {
        const int16x8_t c128 = vdupq_n_s16(128);
        for (; x + 8 <= w; x += 8) {
            int16x8_t yv = vmovl_s8(vld1_s8(y + x));
            int16x8_t bv = vmovl_s8(vld1_s8(b + x));
            int16x8_t rv = vmovl_s8(vld1_s8(r + x));
            int16x8_t t1 = vshrq_n_s16(bv, 2);
            int16x8_t t2 = vaddq_s16(rv, vshrq_n_s16(rv, 1));
            int16x8_t y128 = vaddq_s16(yv, c128);
            int16x8_t t3 = vsubq_s16(y128, t1);
            int16x8_t tr = vaddq_s16(y128, t2);
            int16x8_t tg = vsubq_s16(t3, vshrq_n_s16(t2, 1));
            int16x8_t tb = vaddq_s16(t3, vshlq_n_s16(bv, 1));
            /* saturating signed int16 -> uint8 0..255 (= clamp255) */
            uint8x8_t ru = vqmovun_s16(tr);
            uint8x8_t gu = vqmovun_s16(tg);
            uint8x8_t bu = vqmovun_s16(tb);
            /* Interleave RGB: vst3 writes 8 pixels as RGBRGB... */
            {
                uint8x8x3_t rgb;
                rgb.val[0] = ru;
                rgb.val[1] = gu;
                rgb.val[2] = bu;
                vst3_u8(dst, rgb);
                dst += 24;
            }
        }
    }
#endif
    for (; x < w; x++) {
        int yv = y[x], bv = b[x], rv = r[x];
        int t1 = bv >> 2;
        int t2 = rv + (rv >> 1);
        int t3 = yv + 128 - t1;
        int tr = yv + 128 + t2;
        int tg = t3 - (t2 >> 1);
        int tb = t3 + (bv << 1);
        dst[0] = (uint8_t)clamp255(tr);
        dst[1] = (uint8_t)clamp255(tg);
        dst[2] = (uint8_t)clamp255(tb);
        dst += 3;
    }
}

/* Gray IW44: RGB = 127 - Y (clamped), planar Y row -> interleaved RGB. */
static void gray_y_row_to_rgb(const int8_t *y, uint8_t *dst, int w)
{
    int x;
    for (x = 0; x < w; x++) {
        uint8_t g = (uint8_t)clamp255(127 - y[x]);
        dst[0] = dst[1] = dst[2] = g;
        dst += 3;
    }
}

static int iw44_render_rgb_impl(iw_pixmap *pm, uint8_t *rgb, int flip)
{
    djvu_ctx *ctx;
    int w, h, row, color;
    size_t plane;
    int8_t *planes;
    int8_t *yp, *bp, *rp;
    if (!pm || !pm->ymap) return -1;
    ctx = pm->ctx;
    w = pm->w; h = pm->h;
    color = djvu_iw44_is_color(pm);
    plane = (size_t)w * (size_t)h;

    /* Planar Y/Cb/Cr (or Y only for gray): contiguous rows for SIMD clamp +
       color convert. map_image still emits bottom-up (DjVu convention). */
    planes = (int8_t *)djvu_alloc(ctx, plane * (color ? 3u : 1u));
    if (!planes) return -1;
    memset(planes, 0, plane * (color ? 3u : 1u));
    yp = planes;
    bp = color ? planes + plane : NULL;
    rp = color ? planes + 2 * plane : NULL;

    if (map_image(ctx, pm->ymap, 0, yp, w, 1, 0) != 0) {
        djvu_free(ctx, planes);
        return -1;
    }
    if (color) {
        if (map_image(ctx, pm->cbmap, 0, bp, w, 1, pm->crcbhalf) != 0 ||
            map_image(ctx, pm->crmap, 0, rp, w, 1, pm->crcbhalf) != 0) {
            djvu_free(ctx, planes);
            return -1;
        }
        for (row = 0; row < h; row++) {
            int src_row = flip ? (h - 1 - row) : row;
            size_t off = (size_t)src_row * (size_t)w;
            ycbcr_row_to_rgb(yp + off, bp + off, rp + off,
                             rgb + (size_t)row * (size_t)w * 3, w);
        }
    } else {
        for (row = 0; row < h; row++) {
            int src_row = flip ? (h - 1 - row) : row;
            size_t off = (size_t)src_row * (size_t)w;
            gray_y_row_to_rgb(yp + off, rgb + (size_t)row * (size_t)w * 3, w);
        }
    }
    djvu_free(ctx, planes);
    return 0;
}

int djvu_iw44_render_rgb(iw_pixmap *pm, uint8_t *rgb)
{
    return iw44_render_rgb_impl(pm, rgb, 1);
}
int djvu_iw44_render_rgb_raw(iw_pixmap *pm, uint8_t *rgb)
{
    return iw44_render_rgb_impl(pm, rgb, 0);
}

/* debug: render a single plane (0=Y,1=Cb,2=Cr) as gray (value+128), using the
   same fast flag the pixmap would use for that plane. */
int djvu_iw44_render_plane(iw_pixmap *pm, int plane, uint8_t *gray)
{
    djvu_ctx *ctx; int w, h, i; int8_t *bytes; iw_map *m; int fast;
    if (!pm) return -1;
    ctx = pm->ctx; w = pm->w; h = pm->h;
    m = plane == 1 ? pm->cbmap : plane == 2 ? pm->crmap : pm->ymap;
    fast = plane == 0 ? 0 : pm->crcbhalf;
    if (!m) return -1;
    bytes = (int8_t *)djvu_alloc(ctx, (size_t)w * h);
    if (!bytes) return -1;
    memset(bytes, 0, (size_t)w * h);
    if (map_image(ctx, m, 0, bytes, w, 1, fast) != 0) { djvu_free(ctx, bytes); return -1; }
    for (i = 0; i < w * h; i++) gray[i] = (uint8_t)clamp255(bytes[i] + 128);
    djvu_free(ctx, bytes);
    return 0;
}

int djvu_iw44_render_gray(iw_pixmap *pm, uint8_t *gray)
{
    djvu_ctx *ctx;
    int w, h, i;
    int8_t *bytes;
    if (!pm || !pm->ymap) return -1;
    ctx = pm->ctx;
    w = pm->w; h = pm->h;
    bytes = (int8_t *)djvu_alloc(ctx, (size_t)w * h);
    if (!bytes) return -1;
    memset(bytes, 0, (size_t)w * h);
    if (map_image(ctx, pm->ymap, 0, bytes, w, 1, 0) != 0) { djvu_free(ctx, bytes); return -1; }
    for (i = 0; i < w * h; i++)  /* flip bottom-up -> top-down */
        gray[(h - 1 - i / w) * w + (i % w)] = (uint8_t)clamp255(bytes[i] + 128);
    djvu_free(ctx, bytes);
    return 0;
}
