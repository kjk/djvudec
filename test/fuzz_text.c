/* fuzz_text.c -- libFuzzer target: hidden-text payload (TXTa / decoded TXTz):
 * u24 text length, UTF-8 text, version, recursive zone tree. Input: [u8 opts]
 * [payload]; the payload becomes the TXTa chunk of a synthesized blank page
 * (dims from opts, so zone y-flipping sees varied page heights). */
#include "fuzz_util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_in in = { data, size };
    unsigned opts = fz_u8(&in);
    fz_buf b = { 0 };
    size_t at = fz_page_begin(&b, 16 + (opts & 15) * 300, 16 + (opts >> 4) * 300, 1);
    djvu_ctx *ctx;
    fz_chunk(&b, "TXTa", in.p, in.n);
    fz_page_end(&b, at);
    ctx = fz_ctx_new(0);
    if (ctx) {
        djvu_doc *doc = b.oom ? NULL : djvu_doc_open(ctx, b.d, b.n);
        if (doc) {
            char *text = djvu_page_text(doc, 0);
            djvu_page_text_zones *zones = djvu_page_text_get_zones(doc, 0);
            if (text) djvu_text_destroy(ctx, text);
            if (zones) djvu_text_zones_destroy(ctx, zones);
            djvu_doc_close(doc);
        }
        fz_ctx_free(ctx);
    }
    free(b.d);
    return 0;
}
