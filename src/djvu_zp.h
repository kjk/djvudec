/* djvu_zp.h -- ZP-Coder binary adaptive arithmetic decoder.
 * Ported (decode path only) from DjvuNet Compression/ZPCodec.cs. */
#ifndef DJVU_ZP_H
#define DJVU_ZP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t p;   /* PValue */
    uint16_t m;   /* MValue */
    uint8_t  up;
    uint8_t  dn;  /* down */
} djvu_zp_table;

extern const djvu_zp_table djvu_zp_default_table[256];

/* A BitContext: a single adaptive state byte. Callers keep arrays of these. */
typedef uint8_t djvu_zp_ctx;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;

    uint32_t a;       /* "avalue" */
    uint32_t code;
    uint32_t fence;
    uint32_t buffer;
    uint8_t  scount;
    uint8_t  delay;
    uint8_t  zbyte;
    int      eof;     /* set when delay underflows (matches C# EOF behaviour) */

    uint32_t p[256];
    uint32_t m[256];
    uint8_t  up[256];
    uint8_t  dn[256];
    int8_t   ffzt[256];
} djvu_zp;

/* Initialize a decoder over [data, data+len). */
void djvu_zp_init(djvu_zp *zp, const uint8_t *data, size_t len);

/* Decode one bit with an adaptive context. */
int djvu_zp_decode(djvu_zp *zp, djvu_zp_ctx *ctx);

/* Decode one bit without a context (pass-through), p = 0x8000 + (a>>1). */
int djvu_zp_decode_pass(djvu_zp *zp);

/* IW44 variant, p = 0x8000 + ((a*3)>>3). */
int djvu_zp_decode_iw(djvu_zp *zp);

#endif /* DJVU_ZP_H */
