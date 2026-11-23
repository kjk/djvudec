/* fuzz_target.c -- libFuzzer entry point for the djvu decoder.
 *
 * Each input is treated as a whole .djvu file: open it, then drive the full
 * decode surface (page info, render at full + subsampled resolution, page
 * text, outline) so malformed bytes reach the ZP/JB2/IW44/BZZ codecs. Built
 * with `clang -fsanitize=address,fuzzer`; libFuzzer supplies main().
 *
 * Single-threaded on purpose: concurrency races are covered by the ASan
 * stress harness (test/djvudec_stress.c). See cmd/fuzz.ts for the driver. */
#include "harness_util.h"

/* Cap work per input so one pathological file can't run unbounded and stall
 * the fuzzer's throughput. */
enum { FUZZ_MAX_PAGES = 20 };

/* Full-resolution pixel budget per input. Unmutated corpus seeds alone were
 * enough to trip libFuzzer's slow-unit report: test041C (14 x 9094x5630) and
 * test076C (18 x 4960x7015) are ~700 Mpixels of full-res composite each,
 * 10-12 s under ASan on a 2-core box (slow-unit-321bf8... / slow-unit-2e9c53...
 * were exactly those seeds, byte for byte). Once the budget is spent, later
 * pages still get the subsampled render, text, zones and links -- every codec
 * is still exercised; only the redundant full-res composite is skipped. */
#define FUZZ_MAX_FULLRES_PIXELS ((unsigned long long)128 << 20)

/* Budgeted allocator: a crafted INFO chunk can declare a ~1-gigapixel page
 * and the decoder faithfully allocates what the header says (mask bitmap +
 * render buffers = multi-GB for one input; oom-9e1907... was this, not a
 * leak). Bound live bytes per input so such inputs fail cleanly through the
 * library's alloc-failure paths (fuzzing those too) instead of tripping
 * libFuzzer's RSS limit. hu_size_malloc records each block's size so the free
 * side can un-count it. */
#define FUZZ_MEM_BUDGET ((size_t)1 << 30) /* 1 GB live per input */

static size_t fuzz_live;

static void *fuzz_alloc(void *user, void *ctx, size_t size)
{
    void *p;
    (void)user; (void)ctx;
    if (size > FUZZ_MEM_BUDGET - fuzz_live) return NULL;
    p = hu_size_malloc(size);
    if (p) fuzz_live += size;
    return p;
}

static void fuzz_free(void *user, void *ctx, void *ptr)
{
    (void)user; (void)ctx;
    fuzz_live -= hu_size_free(ptr);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    djvu_ctx *ctx;
    djvu_doc *doc;
    djvu_outline_item *outline;
    int npages, i;
    unsigned long long fullres_pixels = 0;

    djvu_init();

    fuzz_live = 0;
    ctx = djvu_ctx_new(fuzz_alloc, fuzz_free, NULL, NULL, NULL, NULL);
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
        djvu_page_text_zones *zones;
        djvu_page_links *links;

        /* page_info failure: render anyway (fuzzes that error path) */
        if (djvu_doc_page_info(doc, i, &info) == 0 && info.width > 0 &&
            info.height > 0)
            fullres_pixels += (unsigned long long)info.width * info.height;
        if (fullres_pixels <= FUZZ_MAX_FULLRES_PIXELS) {
            img = djvu_page_render(doc, i, 1);
            if (img) djvu_image_destroy(ctx, img);
        }

        img = djvu_page_render(doc, i, 4);
        if (img) djvu_image_destroy(ctx, img);

        text = djvu_page_text(doc, i);
        if (text) djvu_text_destroy(ctx, text);

        /* separate parsers from the plain-text path above */
        zones = djvu_page_text_get_zones(doc, i);
        if (zones) djvu_text_zones_destroy(ctx, zones);

        links = djvu_page_get_links(doc, i);
        if (links) djvu_page_links_destroy(ctx, links);
    }

    outline = djvu_doc_outline(doc);
    if (outline) djvu_outline_destroy(ctx, outline);

    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    /* Leak check on every platform (LeakSanitizer only exists on Linux/macOS,
       so a leak would otherwise pass the Windows runs). */
    if (fuzz_live != 0) {
        fprintf(stderr, "fuzz_target: leaked %zu bytes\n", fuzz_live);
        abort();
    }
    return 0;
}
