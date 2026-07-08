/* fuzz_target.c -- libFuzzer entry point for the djvu decoder.
 *
 * Each input is treated as a whole .djvu file: open it, then drive the full
 * decode surface (page info, render at full + subsampled resolution, page
 * text, outline) so malformed bytes reach the ZP/JB2/IW44/BZZ codecs. Built
 * with `clang -fsanitize=address,fuzzer`; libFuzzer supplies main().
 *
 * Single-threaded on purpose: concurrency races are covered by the ASan
 * stress harness (test/djvudec_stress.c). See cmd/fuzz.ts for the driver. */
#include "djvu.h"

/* Cap work per input so one pathological file can't run unbounded and stall
 * the fuzzer's throughput. */
enum { FUZZ_MAX_PAGES = 20 };

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    djvu_ctx *ctx;
    djvu_doc *doc;
    djvu_outline_item *outline;
    int npages, i;

    djvu_init();

    ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, NULL, NULL);
    if (!ctx) return 0;

    doc = djvu_doc_open(ctx, data, size);
    if (!doc) {
        djvu_ctx_free(ctx);
        return 0;
    }

    npages = djvu_doc_page_count(doc);
    if (npages > FUZZ_MAX_PAGES) npages = FUZZ_MAX_PAGES;

    for (i = 0; i < npages; i++) {
        djvu_page_info info;
        djvu_image *img;
        char *text;

        djvu_doc_page_info(doc, i, &info);

        img = djvu_page_render(doc, i, 1);
        if (img) djvu_image_destroy(ctx, img);

        img = djvu_page_render(doc, i, 4);
        if (img) djvu_image_destroy(ctx, img);

        text = djvu_page_text(doc, i);
        if (text) djvu_text_destroy(ctx, text);
    }

    outline = djvu_doc_outline(doc);
    if (outline) djvu_outline_destroy(ctx, outline);

    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    return 0;
}
