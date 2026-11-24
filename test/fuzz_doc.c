/* fuzz_doc.c -- libFuzzer target: a whole .djvu file.
 *
 * Opens the input as a document and drives the full public decode surface
 * (page info, render + render_into at full and subsampled resolution, text,
 * zones, links, page ids/titles/name lookup, outline). This is the target for
 * the container/DIRM/INCL/shared-dict structure and cross-layer composition;
 * the per-codec targets (fuzz_jb2.c, fuzz_iw44.c, fuzz_bzz.c, fuzz_text.c,
 * fuzz_annot.c, fuzz_outline.c) reach the codecs at far higher exec/s.
 * Driven by cmd/fuzz.ts.
 *
 * Single-threaded on purpose: concurrency races are covered by the ASan
 * stress harness (test/djvudec_stress.c). */
#include "fuzz_util.h"

/* Cap work per input so one pathological file can't run unbounded and stall
 * the fuzzer's throughput. */
enum { FUZZ_MAX_PAGES = 20 };

/* Full-resolution pixel budget per input. Unmutated corpus seeds alone were
 * enough to trip libFuzzer's slow-unit report: test041C (14 x 9094x5630) and
 * test076C (18 x 4960x7015) are ~700 Mpixels of full-res composite each,
 * 10-12 s under ASan on a 2-core box. Pages over FZ_MAX_FULLRES_PAGE_PIXELS or
 * past this budget get only the subsampled render (plus text, zones, links) --
 * every codec is still exercised; only the redundant full-res composite is
 * skipped. */
#define FUZZ_MAX_FULLRES_PIXELS ((unsigned long long)4 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    djvu_ctx *ctx;
    djvu_doc *doc;
    djvu_outline_item *outline;
    int npages, i;
    unsigned long long budget = FUZZ_MAX_FULLRES_PIXELS;
    /* ctx options from the last byte: chunk payload in real files, so the
       seeds spread over every configuration and mutations flip it. */
    unsigned opts = size ? data[size - 1] : 0;

    ctx = fz_ctx_new(opts);
    if (!ctx) return 0;

    doc = djvu_doc_open(ctx, data, size);
    if (!doc) {
        fz_ctx_free(ctx);
        return 0;
    }

    npages = djvu_doc_page_count(doc);
    if (npages > FUZZ_MAX_PAGES) npages = FUZZ_MAX_PAGES;

    for (i = 0; i < npages; i++) {
        const char *id = djvu_doc_page_id(doc, i);
        (void)djvu_doc_page_title(doc, i);
        if (id) (void)djvu_doc_page_by_name(doc, id);
        fz_drive_page(doc, ctx, i, 0, (opts >> 4) & 1, &budget);
    }
    (void)djvu_doc_page_by_name(doc, "#1");

    outline = djvu_doc_outline(doc);
    if (outline) djvu_outline_destroy(ctx, outline);

    djvu_doc_close(doc);
    fz_ctx_free(ctx);
    return 0;
}
