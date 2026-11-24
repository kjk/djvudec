/* fuzz_diff.c -- differential libFuzzer target: our decoder vs DjVuLibre.
 *
 * The input is a whole .djvu file (IFF-aware mutations, fuzz_mutator.h). On
 * small pages that BOTH decoders render, the full-resolution outputs must be
 * identical up to the one accepted difference (the palette-overlap rule,
 * oracle_compare.h); anything else aborts, so libFuzzer saves the input to
 * fuzz/crashes/diff/. This finds wrong output, not just memory errors -- the
 * fuzzing counterpart of `cmd/tests.ts`.
 *
 * Only pages both sides render WITHOUT ANY DIAGNOSTIC are compared: on
 * damaged input the two decoders legitimately differ in how they recover
 * (DjVuLibre throws at the first bad chunk and keeps what it has; we may
 * skip it), so an error or warning on either side -- or a page-count
 * disagreement -- is not a finding. A finding means both decoders accepted
 * the input as valid and still rendered it differently.
 *
 * Also skipped: inputs where one of our ZP streams ran past DjVuLibre's
 * EndOfFile point (djvu_fuzz_zp_eof, a DJVU_FUZZ_HOOKS counter). DjVuLibre's
 * ZPCodec throws 25 bytes past the end of a truncated JB2/IW44/BZZ stream,
 * keeping what it decoded so far without reporting an error; we pad with 0xff
 * and keep decoding. That is a known, separate divergence on truncated data,
 * not something every fuzz run should rediscover.
 *
 * Triage: a crash or hang whose stack is inside libdjvu is a DjVuLibre bug,
 * not ours (the oracle is uninstrumented C++); fuzz.ts runs this target with
 * -detect_leaks=0 because DjVuLibre leaks on some error paths. */
#include "fuzz_util.h"
#include "oracle_compare.h"
#include "fuzz_mutator.h"

int fz_ddjvu_open(const uint8_t *data, size_t len);
int fz_ddjvu_render(int page0, int want_rgb, bench_render *out);
void fz_ddjvu_close(void);
void bench_render_free(bench_render *img);
int fz_ddjvu_errors(void);

enum { DIFF_MAX_PAGES = 4 };

/* Differing pixels (any format; image_compare only counts RGB palette pages)
   and the first differing pixel's position. */
static size_t diff_pixels(const djvu_image *mine, const bench_render *ref, int *fx, int *fy)
{
    size_t n = 0;
    int x, y, bpp = (int)mine->format;
    *fx = *fy = -1;
    if (mine->width != ref->width || mine->height != ref->height || bpp != ref->bps)
        return (size_t)-1;
    for (y = 0; y < mine->height; y++) {
        const uint8_t *a = mine->data + (size_t)y * (size_t)mine->stride;
        const uint8_t *b = ref->data + (size_t)y * (size_t)ref->rowsize;
        for (x = 0; x < mine->width; x++, a += bpp, b += bpp)
            if (memcmp(a, b, (size_t)bpp) != 0) {
                if (!n) { *fx = x; *fy = y; }
                n++;
            }
    }
    return n;
}

/* FUZZ_DIFF_DUMP=prefix: write <prefix>ours.pnm / <prefix>ddjvu.pnm on a
   mismatch (both BGR -> written as RGB). */
static void dump_pair(const djvu_image *mine, const bench_render *ref)
{
    const char *pre = getenv("FUZZ_DIFF_DUMP");
    char path[1024];
    djvu_image r;
    if (!pre) return;
    snprintf(path, sizeof(path), "%sours.pnm", pre);
    hu_write_pnm(path, mine, 1);
    r.width = ref->width; r.height = ref->height;
    r.format = (djvu_format)ref->bps; r.stride = ref->rowsize; r.data = ref->data;
    snprintf(path, sizeof(path), "%sddjvu.pnm", pre);
    hu_write_pnm(path, &r, 1);
}
#define DIFF_MAX_PAGE_PIXELS ((unsigned long long)4 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    djvu_ctx *ctx;
    djvu_doc *doc;
    int npages, i, ref_pages, cand[DIFF_MAX_PAGES], ncand = 0;

    djvu_fuzz_zp_eof = 0;
    ctx = fz_ctx_new(FZ_OPT_BGR); /* compare BGR with ddjvu's BGR24 */
    if (!ctx) return 0;
    doc = djvu_doc_open(ctx, data, size);
    if (!doc) {
        fz_ctx_free(ctx);
        return 0;
    }
    npages = djvu_doc_page_count(doc);
    for (i = 0; i < npages && ncand < DIFF_MAX_PAGES; i++) {
        djvu_page_info info;
        if (djvu_doc_page_info(doc, i, &info) == 0 && info.width > 0 && info.height > 0 &&
            (unsigned long long)info.width * (unsigned long long)info.height <=
                DIFF_MAX_PAGE_PIXELS)
            cand[ncand++] = i;
    }
    ref_pages = ncand ? fz_ddjvu_open(data, size) : -1;
    if (ref_pages == npages) {
        for (i = 0; i < ncand; i++) {
            int p = cand[i];
            djvu_image *mine = djvu_page_render(doc, p, 1);
            bench_render ref;
            if (!mine) continue;
            if (fz_ddjvu_render(p, mine->format == DJVU_FORMAT_RGB24, &ref) == 0) {
                size_t ndiff = 0;
                if (fz_diag == 0 && fz_ddjvu_errors() == 0 && djvu_fuzz_zp_eof == 0 &&
                    image_compare(doc, p, mine, &ref, &ndiff) == IMG_DIFF) {
                    int fx, fy;
                    ndiff = diff_pixels(mine, &ref, &fx, &fy);
                    fprintf(stderr,
                            "fuzz_diff: page %d differs from DjVuLibre: ours %dx%d fmt %d, "
                            "ddjvu %dx%d bps %d, %zd differing pixels, first at (%d,%d), zp_eof %d\n",
                            p + 1, mine->width, mine->height, (int)mine->format,
                            ref.width, ref.height, ref.bps, (ptrdiff_t)ndiff, fx, fy, djvu_fuzz_zp_eof);
                    dump_pair(mine, &ref);
                    abort();
                }
                bench_render_free(&ref);
            }
            djvu_image_destroy(ctx, mine);
        }
    }
    fz_ddjvu_close();
    djvu_doc_close(doc);
    fz_ctx_free(ctx);
    return 0;
}
