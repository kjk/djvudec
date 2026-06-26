/* zpcodec.c -- ZP-Coder decoder. Ported from DjvuNet Compression/ZPCodec.cs.
 * Non-ZCODER ("Z-Coder") variant, decode path only. */
#include "djvu_internal.h"

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