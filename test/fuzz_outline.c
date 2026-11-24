/* fuzz_outline.c -- libFuzzer target: decoded NAVM bookmark payload (u16
 * count, then recursive items with u24-length title/url strings). Input is
 * the BZZ-decompressed payload, fed to djvu_outline_parse against a trivial
 * one-page document (url resolution: "#N" page numbers / ids). */
#include "fuzz_util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_buf b = { 0 };
    size_t at = fz_page_begin(&b, 100, 100, 1);
    djvu_ctx *ctx;
    fz_page_end(&b, at);
    ctx = fz_ctx_new(0);
    if (ctx) {
        djvu_doc *doc = b.oom ? NULL : djvu_doc_open(ctx, b.d, b.n);
        if (doc) {
            djvu_outline_item *root = djvu_outline_parse(doc, data, size);
            if (root) djvu_outline_destroy(ctx, root);
            djvu_doc_close(doc);
        }
        fz_ctx_free(ctx);
    }
    free(b.d);
    return 0;
}
