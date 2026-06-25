/* bzz.c -- BZZ decompression. Ported from DjvuNet Compression/BSInputStream.cs
 * (Decode/DecodeRaw/DecodeBinary) and BSBaseStream.cs constants. */
#include "djvu_internal.h"
#include <stdlib.h>
#include <string.h>

#define BZZ_MAXBLOCK 4096   /* in KiB */
#define BZZ_FREQMAX  4
#define BZZ_CTXIDS   3

typedef struct {
    djvu_zp zp;
    djvu_zp_ctx cxt[300];
    uint8_t *block;       /* current decoded block buffer */
    int block_cap;
    int *pos;             /* BWT position buffer */
    int pos_cap;
} bzz_dec;

static int bzz_decode_raw(bzz_dec *d, int bits)
{
    int n = 1;
    int m = (1 << bits);
    while (n < m) {
        int b = djvu_zp_decode_pass(&d->zp);
        n = (n << 1) | b;
    }
    return n - m;
}

static int bzz_decode_binary(bzz_dec *d, int ctxoff, int bits)
{
    int n = 1;
    int m = (1 << bits);
    ctxoff--;
    while (n < m) {
        int b = djvu_zp_decode(&d->zp, &d->cxt[ctxoff + n]);
        n = (n << 1) | b;
    }
    return n - m;
}

/* Decode one block. Returns block byte count (>=0), or -1 on corruption.
   Decoded bytes are placed at d->block[0..count). Returns 0 at EOF. */
static int bzz_decode_block(bzz_dec *d, djvu_ctx *ctx)
{
    int size = bzz_decode_raw(d, 24);
    int fshift = 0, fadd = 4, mtfno = 3, markerpos = -1;
    int i, k, last, j2;
    uint8_t mtf[256];
    int freq[BZZ_FREQMAX];
    int count[256];
    uint8_t *data;

    if (size == 0)
        return 0;
    if (size > BZZ_MAXBLOCK * 1024) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "bzz: corrupt block size");
        return -1;
    }

    if (d->block_cap < size) {
        djvu_free(ctx, d->block);
        d->block = (uint8_t *)djvu_alloc(ctx, (size_t)size);
        if (!d->block) return -1;
        d->block_cap = size;
    }
    data = d->block;

    /* decoder estimation speed */
    if (djvu_zp_decode_pass(&d->zp) != 0) {
        fshift++;
        if (djvu_zp_decode_pass(&d->zp) != 0)
            fshift++;
    }

    /* prepare quasi-MTF */
    for (i = 0; i < 256; i++) mtf[i] = (uint8_t)i;
    for (i = 0; i < BZZ_FREQMAX; i++) freq[i] = 0;

    for (i = 0; i < size; i++) {
        int ctxid = BZZ_CTXIDS - 1;
        int ctxoff = 0;
        int fc;
        if (ctxid > mtfno) ctxid = mtfno;

        if (djvu_zp_decode(&d->zp, &d->cxt[ctxoff + ctxid]) != 0) {
            mtfno = 0; data[i] = mtf[mtfno];
        } else if ((ctxoff += BZZ_CTXIDS),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + ctxid]) != 0) {
            mtfno = 1; data[i] = mtf[mtfno];
        } else if ((ctxoff += BZZ_CTXIDS),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 2 + bzz_decode_binary(d, ctxoff + 1, 1); data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 1)),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 4 + bzz_decode_binary(d, ctxoff + 1, 2); data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 3)),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 8 + bzz_decode_binary(d, ctxoff + 1, 3); data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 7)),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 16 + bzz_decode_binary(d, ctxoff + 1, 4); data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 15)),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 32 + bzz_decode_binary(d, ctxoff + 1, 5); data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 31)),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 64 + bzz_decode_binary(d, ctxoff + 1, 6); data[i] = mtf[mtfno];
        } else if ((ctxoff += (1 + 63)),
                   djvu_zp_decode(&d->zp, &d->cxt[ctxoff + 0]) != 0) {
            mtfno = 128 + bzz_decode_binary(d, ctxoff + 1, 7); data[i] = mtf[mtfno];
        } else {
            mtfno = 256; data[i] = 0; markerpos = i;
            continue;
        }

        /* rotate mtf according to empirical frequencies */
        fadd = fadd + (fadd >> fshift);
        if (fadd > 0x10000000) {
            fadd >>= 24;
            freq[0] >>= 24; freq[1] >>= 24; freq[2] >>= 24; freq[3] >>= 24;
        }
        fc = fadd;
        if (mtfno < BZZ_FREQMAX) fc += freq[mtfno];
        for (k = mtfno; k >= BZZ_FREQMAX; k--)
            mtf[k] = mtf[k - 1];
        for (; k > 0 && (uint32_t)fc >= (uint32_t)freq[k - 1]; k--) {
            mtf[k] = mtf[k - 1];
            freq[k] = freq[k - 1];
        }
        mtf[k] = data[i];
        freq[k] = fc;
    }

    /* reconstruct the string (inverse Burrows-Wheeler) */
    if (markerpos < 1 || markerpos >= size) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "bzz: corrupt (markerpos)");
        return -1;
    }

    if (d->pos_cap < size) {
        djvu_free(ctx, d->pos);
        d->pos = (int *)djvu_alloc(ctx, sizeof(int) * (size_t)size);
        if (!d->pos) return -1;
        d->pos_cap = size;
    }

    for (i = 0; i < 256; i++) count[i] = 0;
    for (i = 0; i < markerpos; i++) {
        signed char c = (signed char)data[i];
        d->pos[i] = (c << 24) | (count[0xff & c] & 0xffffff);
        count[0xff & c]++;
    }
    for (i = markerpos + 1; i < size; i++) {
        signed char c = (signed char)data[i];
        d->pos[i] = (c << 24) | (count[0xff & c] & 0xffffff);
        count[0xff & c]++;
    }

    last = 1;
    for (i = 0; i < 256; i++) {
        int tmp = count[i];
        count[i] = last;
        last += tmp;
    }

    j2 = 0;
    last = size - 1;
    while (last > 0) {
        int n = d->pos[j2];
        signed char c = (signed char)(d->pos[j2] >> 24);
        data[--last] = (uint8_t)c;
        j2 = count[0xff & c] + (n & 0xffffff);
    }

    if (j2 != markerpos) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "bzz: corrupt (BWT)");
        return -1;
    }

    /* effective block excludes the marker byte: size-1 bytes at data[0..) */
    return size - 1;
}

uint8_t *djvu_bzz_decode_all(djvu_ctx *ctx, const uint8_t *data, size_t len,
                             size_t *out_len)
{
    bzz_dec d;
    uint8_t *out = NULL;
    size_t out_cap = 0, out_size = 0;

    memset(&d, 0, sizeof(d));
    djvu_zp_init(&d.zp, data, len);

    for (;;) {
        int n = bzz_decode_block(&d, ctx);
        if (n < 0) { djvu_free(ctx, out); out = NULL; out_size = 0; break; }
        if (n == 0) break;   /* EOF block */
        if (out_size + (size_t)n + 1 > out_cap) {
            size_t ncap = out_cap ? out_cap * 2 : 65536;
            uint8_t *no;
            while (ncap < out_size + (size_t)n + 1) ncap *= 2;
            no = (uint8_t *)djvu_alloc(ctx, ncap);
            if (!no) { djvu_free(ctx, out); out = NULL; out_size = 0; break; }
            if (out) { memcpy(no, out, out_size); djvu_free(ctx, out); }
            out = no; out_cap = ncap;
        }
        memcpy(out + out_size, d.block, (size_t)n);
        out_size += (size_t)n;
    }

    djvu_free(ctx, d.block);
    djvu_free(ctx, d.pos);
    if (out) {
        out[out_size] = 0;
        if (out_len) *out_len = out_size;
    }
    return out;
}
