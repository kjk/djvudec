/* fuzz_bzz.c -- libFuzzer target: BZZ (Burrows-Wheeler + ZP) decompression,
 * the codec behind DIRM, TXTz, ANTz, NAVM and FGbz indices. Input is a raw
 * BZZ stream. */
#include "fuzz_util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    djvu_ctx *ctx = fz_ctx_new(0);
    size_t n;
    uint8_t *out;
    if (!ctx) return 0;
    out = djvu_bzz_decode_all(ctx, data, size, &n);
    if (out) djvu_free(ctx, out);
    fz_ctx_free(ctx);
    return 0;
}
