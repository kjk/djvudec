/* djvu_bzz.h -- BZZ (Burrows-Wheeler + ZP) decompression.
 * Ported from DjvuNet Compression/BSInputStream.cs (decode path). */
#ifndef DJVU_BZZ_H
#define DJVU_BZZ_H

#include "djvu_internal.h"

/* Decompress an entire BZZ stream at [data, data+len) into a freshly
   allocated buffer (NUL-terminated for convenience; the NUL is not counted
   in *out_len). Returns NULL on error. Free with djvu_free(). */
uint8_t *djvu_bzz_decode_all(djvu_ctx *ctx, const uint8_t *data, size_t len,
                             size_t *out_len);

#endif /* DJVU_BZZ_H */
