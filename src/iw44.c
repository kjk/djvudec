/* iw44.c -- IW44 wavelet decoder. Ported from DjvuNet Wavelet/
 * {InterWaveCodec,InterWaveDecoder,InterWaveMap,InterWaveBlock,
 *  InterWavePixelMap,InterWavePixelMapDecoder,InterWaveTransform}.cs */
#include "djvu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const int16_t djvu_iw44_zigzag[1024];

/* ---------- block: 64 sparse buckets of 16 coefficients ---------- */

typedef struct {
    int16_t *buckets[64];   /* NULL or array of 16 */
} iw_block;

typedef struct {
    int w, h, bw, bh, nb;
    iw_block *blocks;
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
    int i;
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->w = w; m->h = h;
    m->bw = (w + 0x20 - 1) & ~0x1f;
    m->bh = (h + 0x20 - 1) & ~0x1f;
    m->nb = (m->bw * m->bh) / 1024;
    m->blocks = (iw_block *)djvu_alloc(ctx, sizeof(iw_block) * m->nb);
    if (!m->blocks) { djvu_free(ctx, m); return NULL; }
    for (i = 0; i < m->nb; i++)
        memset(&m->blocks[i], 0, sizeof(iw_block));
    return m;
}

static void map_free(djvu_ctx *ctx, iw_map *m)
{
    int i, b;
    if (!m) return;
    for (i = 0; i < m->nb; i++)
        for (b = 0; b < 64; b++)
            djvu_free(ctx, m->blocks[i].buckets[b]);
    djvu_free(ctx, m->blocks);
    djvu_free(ctx, m);
}

static int16_t *block_get(iw_block *blk, int n) { return blk->buckets[n]; }

static int16_t *block_get_init(djvu_ctx *ctx, iw_block *blk, int n)
{
    if (!blk->buckets[n]) {
        blk->buckets[n] = (int16_t *)djvu_alloc(ctx, sizeof(int16_t) * 16);
        if (blk->buckets[n]) memset(blk->buckets[n], 0, sizeof(int16_t) * 16);
    }
    return blk->buckets[n];
}

/* write the 32x32 lift block (1024 coeffs, zigzag-scattered) for `blk` */
static void write_lift_block(iw_block *blk, int16_t *coeff)
{
    int n = 0, n1, n2;
    memset(coeff, 0, sizeof(int16_t) * 1024);
    for (n1 = 0; n1 < 64; n1++) {
        int16_t *d = block_get(blk, n1);
        if (d) {
            for (n2 = 0; n2 < 16; n2++, n++)
                coeff[djvu_iw44_zigzag[n]] = d[n2];
        } else {
            n += 16;
        }
    }
}

/* ---------- inverse wavelet transform (ported from DjVuLibre IW44Image.cpp
 * filter_bv / filter_bh -- the canonical reference) ---------- */

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
                while (q < e) {
                    int a = (int)q[-s] + (int)q[s];
                    int b = (int)q[-s3] + (int)q[s3];
                    *q = (int16_t)(*q - (((a << 3) + a - b + 16) >> 5));
                    q += scale;
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
                while (q < e) {
                    int a = (int)q[-s] + (int)q[s];
                    int b = (int)q[-s3] + (int)q[s3];
                    *q = (int16_t)(*q + (((a << 3) + a - b + 8) >> 4));
                    q += scale;
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

static void filter_bh(int16_t *p, int w, int h, int rowsize, int scale)
{
    int y = 0;
    int s = scale;
    int s3 = s + s + s;
    rowsize *= scale;
    while (y < h) {
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
        y += scale;
        p += rowsize;
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
    int16_t liftblock[1024];
    int blockidx = 0, i, j, ii, p1idx, ppidx, pidx = 0;
    if (!data16) return NULL;
    memset(data16, 0, sizeof(int16_t) * n);

    for (i = 0; i < m->bh; i += 32, pidx += 32 * m->bw) {
        for (j = 0; j < m->bw; j += 32) {
            write_lift_block(&m->blocks[blockidx], liftblock);
            blockidx++;
            ppidx = pidx + j;
            for (ii = 0, p1idx = 0; ii < 32; ii++, p1idx += 32, ppidx += m->bw)
                memcpy(data16 + ppidx, liftblock + p1idx, sizeof(int16_t) * 32);
        }
    }
    return data16;
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
    for (i = 0, rowidx = index; i < m->h; i++, rowidx += rowsize, pidx += m->bw) {
        for (j = 0, pixidx = rowidx; j < m->w; j++, pixidx += pixsep) {
            int x = (data16[pidx + j] + 32) >> 6;
            if (x < -128) x = -128;
            else if (x > 127) x = 127;
            img8[pixidx] = (int8_t)x;
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

static void decode_buckets(iw_codec *c, djvu_zp *zp, int bit, int band,
                           iw_block *blk, int fbucket, int nbucket)
{
    int thres = c->quant_high[band];
    int bbstate = 0;
    int8_t *cstate = c->coeff_state;
    int cidx = 0, buckno, i;

    (void)bit;
    for (buckno = 0; buckno < nbucket; buckno++, cidx += 16) {
        int bstatetmp = 0;
        int16_t *pcoeff = block_get(blk, fbucket + buckno);
        if (pcoeff == NULL) {
            bstatetmp = 8;
        } else {
            for (i = 0; i < 16; i++) {
                int cstatetmp = cstate[cidx + i] & 1;
                if (cstatetmp == 0)
                    cstatetmp |= (pcoeff[i] != 0) ? 2 : 8;
                cstate[cidx + i] = (int8_t)cstatetmp;
                bstatetmp |= cstatetmp;
            }
        }
        c->bucket_state[buckno] = (int8_t)bstatetmp;
        bbstate |= bstatetmp;
    }

    if (nbucket < 16 || (bbstate & 2) != 0) {
        bbstate |= 4;
    } else if ((bbstate & 8) != 0) {
        if (djvu_zp_decode(zp, &c->ctx_root) != 0)
            bbstate |= 4;
    }

    if ((bbstate & 4) != 0) {
        for (buckno = 0; buckno < nbucket; buckno++) {
            if ((c->bucket_state[buckno] & 8) != 0) {
                int ctx = 0;
                if (band > 0) {
                    int k = (fbucket + buckno) << 2;
                    int16_t *b = block_get(blk, k >> 4);
                    if (b != NULL) {
                        k &= 0xf;
                        if (b[k] != 0) ctx++;
                        if (b[k + 1] != 0) ctx++;
                        if (b[k + 2] != 0) ctx++;
                        if (ctx < 3 && b[k + 3] != 0) ctx++;
                    }
                }
                if ((bbstate & 2) != 0) ctx |= 4;
                if (djvu_zp_decode(zp, &c->ctx_bucket[band][ctx]) != 0)
                    c->bucket_state[buckno] |= 4;
            }
        }
    }

    if ((bbstate & 4) != 0) {
        cstate = c->coeff_state; cidx = 0;
        for (buckno = 0; buckno < nbucket; buckno++, cidx += 16) {
            if ((c->bucket_state[buckno] & 4) != 0) {
                int16_t *pcoeff = block_get(blk, fbucket + buckno);
                int gotcha = 0, maxgotcha = 7;
                if (pcoeff == NULL) {
                    pcoeff = block_get_init(c->ctx, blk, fbucket + buckno);
                    for (i = 0; i < 16; i++)
                        if ((cstate[cidx + i] & 1) == 0) cstate[cidx + i] = 8;
                }
                for (i = 0; i < 16; i++)
                    if ((cstate[cidx + i] & 8) != 0) gotcha++;
                for (i = 0; i < 16; i++) {
                    if ((cstate[cidx + i] & 8) != 0) {
                        int ctx, coeff, halfthres;
                        if (band == 0) thres = c->quant_low[i];
                        ctx = (gotcha >= maxgotcha) ? maxgotcha : gotcha;
                        if ((c->bucket_state[buckno] & 2) != 0) ctx |= 8;
                        if (djvu_zp_decode(zp, &c->ctx_start[ctx]) != 0) {
                            cstate[cidx + i] |= 4;
                            halfthres = thres >> 1;
                            coeff = (thres + halfthres) - (halfthres >> 2);
                            if (djvu_zp_decode_iw(zp) != 0)
                                pcoeff[i] = (int16_t)(-coeff);
                            else
                                pcoeff[i] = (int16_t)coeff;
                        }
                        if ((cstate[cidx + i] & 4) != 0) gotcha = 0;
                        else if (gotcha > 0) gotcha--;
                    }
                }
            }
        }
    }

    if ((bbstate & 2) != 0) {
        cstate = c->coeff_state; cidx = 0;
        for (buckno = 0; buckno < nbucket; buckno++, cidx += 16) {
            if ((c->bucket_state[buckno] & 2) != 0) {
                int16_t *pcoeff = block_get(blk, fbucket + buckno);
                for (i = 0; i < 16; i++) {
                    if ((cstate[cidx + i] & 2) != 0) {
                        int coeff = pcoeff[i];
                        if (coeff < 0) coeff = -coeff;
                        if (band == 0) thres = c->quant_low[i];
                        if (coeff <= (3 * thres)) {
                            coeff += (thres >> 2);
                            if (djvu_zp_decode(zp, &c->ctx_mant) != 0)
                                coeff += (thres >> 1);
                            else
                                coeff = (coeff - thres) + (thres >> 1);
                        } else {
                            if (djvu_zp_decode_iw(zp) != 0)
                                coeff += (thres >> 1);
                            else
                                coeff = (coeff - thres) + (thres >> 1);
                        }
                        if (pcoeff[i] > 0) pcoeff[i] = (int16_t)coeff;
                        else pcoeff[i] = (int16_t)(-coeff);
                    }
                }
            }
        }
    }
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
    pm->crcbdelay = 10;
    return pm;
}

void djvu_iw44_free(iw_pixmap *pm)
{
    djvu_ctx *ctx;
    if (!pm) return;
    ctx = pm->ctx;
    map_free(ctx, pm->ymap);
    map_free(ctx, pm->cbmap);
    map_free(ctx, pm->crmap);
    djvu_free(ctx, pm->yc);
    djvu_free(ctx, pm->cbc);
    djvu_free(ctx, pm->crc);
    djvu_free(ctx, pm);
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

static int iw44_render_rgb_impl(iw_pixmap *pm, uint8_t *rgb, int flip)
{
    djvu_ctx *ctx;
    int w, h, i, color;
    int8_t *bytes;
    if (!pm || !pm->ymap) return -1;
    ctx = pm->ctx;
    w = pm->w; h = pm->h;
    color = djvu_iw44_is_color(pm);

    bytes = (int8_t *)djvu_alloc(ctx, (size_t)w * h * 3);
    if (!bytes) return -1;
    memset(bytes, 0, (size_t)w * h * 3);

    /* map_image produces rows bottom-up (DjVu convention); emit top-down. */
    if (map_image(ctx, pm->ymap, 0, bytes, w * 3, 3, 0) != 0) { djvu_free(ctx, bytes); return -1; }
    if (color) {
        map_image(ctx, pm->cbmap, 1, bytes, w * 3, 3, pm->crcbhalf);
        map_image(ctx, pm->crmap, 2, bytes, w * 3, 3, pm->crcbhalf);
        for (i = 0; i < w * h; i++) {
            int8_t *q = bytes + (size_t)i * 3;
            int yv = q[0], bv = q[1], rv = q[2];
            int t1 = bv >> 2;
            int t2 = rv + (rv >> 1);
            int t3 = yv + 128 - t1;
            int tr = yv + 128 + t2;
            int tg = t3 - (t2 >> 1);
            int tb = t3 + (bv << 1);
            size_t o = (size_t)(flip ? (h - 1 - i / w) * w + (i % w) : i) * 3;
            rgb[o + 0] = (uint8_t)clamp255(tr);
            rgb[o + 1] = (uint8_t)clamp255(tg);
            rgb[o + 2] = (uint8_t)clamp255(tb);
        }
    } else {
        for (i = 0; i < w * h; i++) {
            int g = clamp255(127 - bytes[(size_t)i * 3]);  /* gray = 127 - Y */
            size_t o = (size_t)(flip ? (h - 1 - i / w) * w + (i % w) : i) * 3;
            rgb[o + 0] = rgb[o + 1] = rgb[o + 2] = (uint8_t)g;
        }
    }
    djvu_free(ctx, bytes);
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
