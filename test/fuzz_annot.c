/* fuzz_annot.c -- libFuzzer target: annotation S-expressions (ANTa / decoded
 * ANTz) -> maparea hyperlinks. Input is the raw annotation text; it becomes
 * the ANTa chunk of a synthesized blank page. */
#include "fuzz_util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_buf b = { 0 };
    size_t at = fz_page_begin(&b, 2550, 3300, 1);
    djvu_ctx *ctx;
    fz_chunk(&b, "ANTa", data, size);
    fz_page_end(&b, at);
    ctx = fz_ctx_new(0);
    if (ctx) {
        djvu_doc *doc = b.oom ? NULL : djvu_doc_open(ctx, b.d, b.n);
        if (doc) {
            djvu_page_links *links = djvu_page_get_links(doc, 0);
            if (links) djvu_page_links_destroy(ctx, links);
            djvu_doc_close(doc);
        }
        fz_ctx_free(ctx);
    }
    free(b.d);
    return 0;
}
