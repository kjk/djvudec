/* zpcodec.c -- ZP-Coder decoder. Ported from DjvuNet Compression/ZPCodec.cs.
 * Non-ZCODER ("Z-Coder") variant, decode path only. */
#include "djvu_internal.h"

static int zp_read_byte(djvu_zp *zp)
{
    if (zp->pos < zp->len)
        return zp->data[zp->pos++];
    return -1;
}

static void zp_preload(djvu_zp *zp)
{
    unsigned scount = zp->scount;
    int zbyte = zp->zbyte;
    uint32_t buffer = zp->buffer;

    for (; scount <= 24; scount += 8) {
        int b = zp_read_byte(zp);
        if (b == -1) {
            zbyte = 255;
            if (--zp->delay < 1)
                zp->eof = 1;     /* C# throws EOF; we flag and keep going */
        } else {
            zbyte = b;
        }
        buffer = (buffer << 8) | (uint8_t)zbyte;
    }

    zp->scount = (uint8_t)scount;
    zp->zbyte = (uint8_t)zbyte;
    zp->buffer = buffer;
}

void djvu_zp_init(djvu_zp *zp, const uint8_t *data, size_t len)
{
    int i, j, b;

    zp->data = data;
    zp->len = len;
    zp->pos = 0;
    zp->a = 0;
    zp->buffer = 0;
    zp->scount = 0;
    zp->zbyte = 0;
    zp->eof = 0;

    /* ffz table: number of leading 1-bits in a byte */
    for (i = 0; i < 256; i++) {
        zp->ffzt[i] = 0;
        for (j = i; (j & 0x80) != 0; j <<= 1)
            zp->ffzt[i]++;
    }

    for (i = 0; i < 256; i++) {
        zp->p[i] = djvu_zp_default_table[i].p;
        zp->m[i] = djvu_zp_default_table[i].m;
        zp->up[i] = djvu_zp_default_table[i].up;
        zp->dn[i] = djvu_zp_default_table[i].dn;
    }

    /* DecoderInitialize */
    zp->code = 0xff00;
    b = zp_read_byte(zp);
    zp->code = 0xff00u & (uint32_t)(b << 8);
    b = zp_read_byte(zp);
    zp->zbyte = (uint8_t)(0xff & b);
    zp->code |= zp->zbyte;
    zp->delay = 25;
    zp->scount = 0;
    zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000)
        zp->fence = 0x7fff;
}

static int zp_ffz(djvu_zp *zp, uint32_t x)
{
    return ((x & 0xffffffffu) < 0xff00u)
        ? zp->ffzt[(x >> 8) & 0xff]
        : (zp->ffzt[x & 0xff] + 8);
}

static void zp_renorm_lps(djvu_zp *zp, uint32_t z)
{
    int shift;
    z = 0x10000u - z;
    zp->a += z;
    zp->code += z;
    shift = zp_ffz(zp, zp->a);
    zp->scount -= (uint8_t)shift;
    zp->a = (zp->a << shift) & 0xffff;
    zp->code = ((zp->code << shift) & 0xffff)
             | ((zp->buffer >> zp->scount) & ((1u << shift) - 1));
    if (zp->scount < 16)
        zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000)
        zp->fence = 0x7fff;
}

static void zp_renorm_mps(djvu_zp *zp, uint32_t z)
{
    zp->scount -= 1;
    zp->a = (z << 1) & 0xffff;
    zp->code = ((zp->code << 1) & 0xffff) | ((zp->buffer >> zp->scount) & 1);
    if (zp->scount < 16)
        zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000)
        zp->fence = 0x7fff;
}

/* DecodeSub: full adaptive path with context learning. */
static int zp_decode_sub(djvu_zp *zp, djvu_zp_ctx *ctx, uint32_t z)
{
    int bit = *ctx & 1;
    uint32_t d = 0x6000u + ((z + zp->a) >> 2);
    if (z > d)
        z = d;

    if (z > zp->code) {
        /* LPS */
        *ctx = zp->dn[*ctx];
        zp_renorm_lps(zp, z);
        return bit ^ 1;
    } else {
        /* MPS */
        if (zp->a >= zp->m[*ctx])
            *ctx = zp->up[*ctx];
        zp_renorm_mps(zp, z);
        return bit;
    }
}

int djvu_zp_decode(djvu_zp *zp, djvu_zp_ctx *ctx)
{
    uint32_t z = zp->a + zp->p[*ctx];
    if (z <= zp->fence) {
        zp->a = z;
        return *ctx & 1;
    }
    return zp_decode_sub(zp, ctx, z);
}

/* DecodeSubSimple: no context, no interval adjustment. */
static int zp_decode_sub_simple(djvu_zp *zp, int mps, uint32_t z)
{
    if (z > zp->code) {
        zp_renorm_lps(zp, z);
        return mps ^ 1;
    } else {
        zp_renorm_mps(zp, z);
        return mps;
    }
}

int djvu_zp_decode_pass(djvu_zp *zp)
{
    return zp_decode_sub_simple(zp, 0, 0x8000u + (zp->a >> 1));
}

int djvu_zp_decode_iw(djvu_zp *zp)
{
    return zp_decode_sub_simple(zp, 0, 0x8000u + ((zp->a + zp->a + zp->a) >> 3));
}
