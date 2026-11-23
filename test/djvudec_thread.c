/* djvudec_thread.c -- concurrent stress test for read-only djvu_doc APIs.
 *
 * Calls djvu_init() once, opens a .djvu, then runs randomized page operations
 * from N worker threads. See thread-safety.md.
 *
 *   djvudec_thread [-cpu N] [-nops N] [-abort] file.djvu
 *
 * Defaults: cpu = processor_count/2 (min 1), nops = 256.
 * Each op picks a random page and one of: render, text, zones, links.
 *
 * -abort: cross-thread abort test instead (the way SumatraPDF cancels renders
 * from its UI thread). Workers render random pages through the _abortable
 * variants with a per-render token, while a separate aborter thread requests
 * aborts on in-flight workers' tokens and now and then aborts every render on
 * the ctx (djvu_request_abort). Each render must return either "aborted"
 * (NULL / -1) or output identical to a reference render made up front; after
 * the run every sampled page must still render identically (no stale abort or
 * page-cache state), and at least one render must actually have been aborted
 * and one completed. Built with -tsan this is the data-race check for the
 * abort path. */
#include "harness_util.h"

enum { OP_RENDER = 0, OP_TEXT = 1, OP_ZONES = 2, OP_LINKS = 3, OP_COUNT = 4 };

typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages;
    int nops;
    uint32_t rng;
} worker_arg;

static hu_atomic g_next_op; /* next op index to claim (shared by all workers) */
static hu_atomic g_errors;

static int do_render(djvu_doc *doc, djvu_ctx *ctx, int page)
{
    djvu_image *img = djvu_page_render(doc, page, 1);
    if (!img) return -1;
    djvu_image_destroy(ctx, img);
    return 0;
}

static int do_text(djvu_doc *doc, djvu_ctx *ctx, int page)
{
    char *t = djvu_page_text(doc, page);
    if (t) djvu_text_destroy(ctx, t);
    return 0;
}

static int do_zones(djvu_doc *doc, djvu_ctx *ctx, int page)
{
    djvu_page_text_zones *z = djvu_page_text_get_zones(doc, page);
    if (z) djvu_text_zones_destroy(ctx, z);
    return 0;
}

static int do_links(djvu_doc *doc, djvu_ctx *ctx, int page)
{
    djvu_page_links *L = djvu_page_get_links(doc, page);
    if (L) djvu_page_links_destroy(ctx, L);
    return 0;
}

/* ------------------------------------------------------------ -abort mode */

#define AB_MAX_PAGES 16   /* pages sampled (references are rendered up front) */
static const int ab_subs[2] = { 1, 3 };

typedef struct {
    uint64_t hash;        /* FNV-1a over the rows (stride padding excluded) */
    int w, h, fmt;        /* fmt 0: page renders nothing */
} ab_ref;

typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages, nops;
    uint32_t rng;
    djvu_abort tok;       /* re-armed before each render; aborter requests it */
    hu_atomic busy;       /* 1 while a render with `tok` is in flight */
    hu_atomic target;     /* 1 if the aborter may request `tok` for this render
                             (about half the renders), so some always complete
                             however the timing goes (e.g. under TSan) */
} ab_worker;

static ab_ref g_ab_ref[AB_MAX_PAGES][2];
static hu_atomic g_ab_done, g_ab_aborted, g_ab_completed, g_ab_mismatch;
static hu_atomic g_ab_tok_requests, g_ab_ctx_requests;

static uint64_t ab_hash(const uint8_t *data, int w, int h, int stride, int comp)
{
    uint64_t hsh = 1469598103934665603ull;
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *row = data + (size_t)y * (size_t)stride;
        for (x = 0; x < w * comp; x++)
            hsh = (hsh ^ row[x]) * 1099511628211ull;
    }
    return hsh;
}

static int ab_ref_matches(const ab_ref *r, const uint8_t *data, int w, int h,
                          int stride, int fmt)
{
    return r->fmt == fmt && r->w == w && r->h == h &&
           r->hash == ab_hash(data, w, h, stride, fmt);
}

/* One render through an _abortable variant. Returns 1 completed + identical,
   0 aborted (NULL / -1), -1 completed but different from the reference. */
static int ab_render(ab_worker *w, int page, int si, int into)
{
    const ab_ref *r = &g_ab_ref[page][si];
    int sub = ab_subs[si], rc;

    if (!into) {
        djvu_image *img = djvu_page_render_abortable(w->doc, page, sub, &w->tok);
        if (!img) return 0;
        rc = ab_ref_matches(r, img->data, img->width, img->height, img->stride,
                            (int)img->format) ? 1 : -1;
        djvu_image_destroy(w->ctx, img);
        return rc;
    } else {
        int comp = r->fmt, stride = r->w * comp;
        uint8_t *dst = (uint8_t *)malloc((size_t)stride * (size_t)r->h);
        if (!dst) return -1;
        if (djvu_page_render_into_abortable(w->doc, page, sub, dst, stride, &w->tok) != 0)
            rc = 0;
        else
            rc = ab_ref_matches(r, dst, r->w, r->h, stride, r->fmt) ? 1 : -1;
        free(dst);
        return rc;
    }
}

static void ab_worker_main(void *arg)
{
    ab_worker *w = (ab_worker *)arg;
    int npages = w->npages < AB_MAX_PAGES ? w->npages : AB_MAX_PAGES;

    for (;;) {
        int idx = (int)hu_atomic_inc(&g_next_op) - 1;
        int page, si, into, rc;

        if (idx >= w->nops) break;
        page = (int)(hu_xorshift32(&w->rng) % (uint32_t)npages);
        si = (int)(hu_xorshift32(&w->rng) % 2u);
        into = (int)(hu_xorshift32(&w->rng) % 2u);
        if (g_ab_ref[page][si].fmt == 0) continue;   /* renders nothing */

        djvu_abort_init(&w->tok);
        hu_atomic_store(&w->target, (long)(hu_xorshift32(&w->rng) & 1u));
        hu_atomic_store(&w->busy, 1);
        rc = ab_render(w, page, si, into);
        hu_atomic_store(&w->busy, 0);

        if (rc == 1) hu_atomic_inc(&g_ab_completed);
        else if (rc == 0) hu_atomic_inc(&g_ab_aborted);
        else {
            hu_atomic_inc(&g_ab_mismatch);
            fprintf(stderr, "abort: page %d sub %d %s: output differs from the reference\n",
                    page + 1, ab_subs[si], into ? "render_into" : "render");
        }
    }
}

typedef struct {
    ab_worker *workers;
    int n;
    djvu_ctx *ctx;
    djvu_doc *doc;
    int npages;           /* sampled pages */
    uint32_t rng;
} ab_aborter;

static hu_atomic g_ab_drops;

static void ab_aborter_main(void *arg)
{
    ab_aborter *a = (ab_aborter *)arg;
    long last_bucket = 0;
    while (!hu_atomic_load(&g_ab_done)) {
        uint32_t r = hu_xorshift32(&a->rng);
        /* One ctx-wide abort per 50 renders started: paced by work done, not
           by time, so it hits at most the renders in flight then and cannot
           starve the "completed" outcome however slow renders are (TSan). */
        long bucket = hu_atomic_load(&g_next_op) / 50;
        if (bucket > last_bucket) {
            last_bucket = bucket;
            djvu_request_abort(a->ctx);   /* every in-flight render on the ctx */
            hu_atomic_inc(&g_ab_ctx_requests);
        } else if (r % 64u == 1) {
            /* evict a page's cached layers (SumatraPDF's LRU does this) so
               renders keep re-decoding -- and so aborts land in decodes,
               not only in cheap renders from cache */
            djvu_doc_drop_page_cache(a->doc, (int)((r >> 8) % (uint32_t)a->npages));
            hu_atomic_inc(&g_ab_drops);
        } else {
            ab_worker *w = &a->workers[r % (uint32_t)a->n];
            if (hu_atomic_load(&w->busy) && hu_atomic_load(&w->target)) {
                djvu_abort_request(&w->tok);
                hu_atomic_inc(&g_ab_tok_requests);
            }
        }
        /* vary the timing so aborts land at different points of a render */
        if (r % 8u == 0) hu_sleep_ms(1); else hu_yield();
    }
}

/* Reference render of every sampled page at both subsamples (single-threaded,
   before any abort). fmt stays 0 for a page/subsample that renders nothing. */
static int ab_make_refs(djvu_doc *doc, djvu_ctx *ctx, int npages)
{
    int page, si;
    for (page = 0; page < npages; page++)
        for (si = 0; si < 2; si++) {
            djvu_image *img = djvu_page_render(doc, page, ab_subs[si]);
            ab_ref *r = &g_ab_ref[page][si];
            if (!img) { r->fmt = 0; continue; }
            r->w = img->width;
            r->h = img->height;
            r->fmt = (int)img->format;
            r->hash = ab_hash(img->data, img->width, img->height, img->stride, r->fmt);
            djvu_image_destroy(ctx, img);
        }
    return 0;
}

/* After the run: every sampled page must still render exactly like before. */
static long ab_check_after(djvu_doc *doc, djvu_ctx *ctx, int npages)
{
    long bad = 0;
    int page, si;
    for (page = 0; page < npages; page++)
        for (si = 0; si < 2; si++) {
            const ab_ref *r = &g_ab_ref[page][si];
            djvu_image *img;
            if (r->fmt == 0) continue;
            img = djvu_page_render(doc, page, ab_subs[si]);
            if (!img || !ab_ref_matches(r, img->data, img->width, img->height,
                                        img->stride, (int)img->format)) {
                fprintf(stderr, "abort: page %d sub %d renders differently after the run\n",
                        page + 1, ab_subs[si]);
                bad++;
            }
            if (img) djvu_image_destroy(ctx, img);
        }
    return bad;
}

static int run_abort_mode(djvu_doc *doc, djvu_ctx *ctx, int npages, int ncpu,
                          int nops, const char *path)
{
    int sampled = npages < AB_MAX_PAGES ? npages : AB_MAX_PAGES, i, started = 1;
    ab_worker *workers = (ab_worker *)calloc((size_t)ncpu, sizeof(ab_worker));
    hu_thread *threads = (hu_thread *)calloc((size_t)ncpu, sizeof(hu_thread));
    hu_thread ath;
    ab_aborter ab;
    long after_bad, aborted, completed, mismatch;
    int rc;

    memset(&ath, 0, sizeof(ath));
    if (!workers || !threads) { free(workers); free(threads); return 1; }
    ab_make_refs(doc, ctx, sampled);

    for (i = 0; i < ncpu; i++) {
        workers[i].doc = doc;
        workers[i].ctx = ctx;
        workers[i].npages = npages;
        workers[i].nops = nops;
        workers[i].rng = 0x9e3779b9u ^ (uint32_t)(i + 1) * 0x85ebca6bu;
        djvu_abort_init(&workers[i].tok);
    }
    ab.workers = workers;
    ab.n = ncpu;
    ab.ctx = ctx;
    ab.doc = doc;
    ab.npages = sampled;
    ab.rng = 0x2545f491u;
    if (hu_thread_start(&ath, ab_aborter_main, &ab) != 0) {
        fprintf(stderr, "cannot create the aborter thread\n");
        free(workers); free(threads);
        return 1;
    }
    for (i = 0; i < ncpu; i++)
        if (hu_thread_start(&threads[i], ab_worker_main, &workers[i]) != 0) {
            fprintf(stderr, "cannot create thread %d\n", i);
            started = 0;
            break;
        }
    /* workers first, then stop and join the aborter (it reads the workers'
       tokens, so they must outlive it) */
    hu_threads_join(threads, ncpu);
    hu_atomic_store(&g_ab_done, 1);
    hu_threads_join(&ath, 1);

    after_bad = ab_check_after(doc, ctx, sampled);
    aborted = hu_atomic_load(&g_ab_aborted);
    completed = hu_atomic_load(&g_ab_completed);
    mismatch = hu_atomic_load(&g_ab_mismatch);
    printf("%s: abort mode, %d threads, %d ops: %ld completed, %ld aborted, "
           "%ld mismatched; %ld token + %ld ctx abort requests, %ld cache drops; "
           "%ld pages bad after\n",
           path, ncpu, nops, completed, aborted, mismatch,
           hu_atomic_load(&g_ab_tok_requests), hu_atomic_load(&g_ab_ctx_requests),
           hu_atomic_load(&g_ab_drops), after_bad);
    rc = (!started || mismatch || after_bad) ? 1 : 0;
    if (!rc && (aborted == 0 || completed == 0)) {
        /* would pass vacuously: the test must have exercised both outcomes */
        fprintf(stderr, "abort: need at least one aborted and one completed render "
                        "(raise -nops)\n");
        rc = 1;
    }
    free(threads);
    free(workers);
    return rc;
}

static int run_one_op(worker_arg *w, int op, int page)
{
    switch (op) {
    case OP_RENDER: return do_render(w->doc, w->ctx, page);
    case OP_TEXT:   return do_text(w->doc, w->ctx, page);
    case OP_ZONES:  return do_zones(w->doc, w->ctx, page);
    case OP_LINKS:  return do_links(w->doc, w->ctx, page);
    default:        return -1;
    }
}

static void worker_main(void *arg)
{
    worker_arg *w = (worker_arg *)arg;

    for (;;) {
        int idx = (int)hu_atomic_inc(&g_next_op) - 1;
        int page, op;

        if (idx >= w->nops) break;

        page = (int)(hu_xorshift32(&w->rng) % (uint32_t)w->npages);
        op = (int)(hu_xorshift32(&w->rng) % (uint32_t)OP_COUNT);

        if (run_one_op(w, op, page) != 0)
            hu_atomic_inc(&g_errors);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-cpu N] [-nops N] [-abort] file.djvu\n"
        "  stress-test concurrent render/text/zones/links on one open document\n"
        "  -abort  cross-thread abort test (token + ctx-wide aborts from another thread)\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int ncpu = 0, nops = 256, npages, i, started_all = 1, rc = 1, abort_mode = 0;
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    worker_arg *workers = NULL;
    hu_thread *threads = NULL;
    long errors;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-cpu") && i + 1 < argc)
            ncpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-nops") && i + 1 < argc)
            nops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-abort"))
            abort_mode = 1;
        else if (argv[i][0] == '-')
            { usage(argv[0]); return 2; }
        else
            path = argv[i];
    }

    if (!path) { usage(argv[0]); return 2; }
    if (ncpu <= 0) {
        ncpu = hu_cpu_count() / 2;
        if (ncpu < 1) ncpu = 1;
    }
    if (nops <= 0) nops = 256;

    data = hu_read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    djvu_init();
    ctx = djvu_ctx_new(NULL, NULL, hu_lock_cb, hu_unlock_cb, hu_on_error, NULL);
    if (!ctx) goto done;
    djvu_ctx_set_cache_per_page(ctx, 1);

    doc = djvu_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "djvu_doc_open failed\n");
        goto done;
    }
    npages = djvu_doc_page_count(doc);
    if (npages <= 0) {
        fprintf(stderr, "document has no pages\n");
        goto done;
    }

    if (abort_mode) {
        rc = run_abort_mode(doc, ctx, npages, ncpu, nops, path);
        goto done;
    }

    workers = (worker_arg *)calloc((size_t)ncpu, sizeof(worker_arg));
    threads = (hu_thread *)calloc((size_t)ncpu, sizeof(hu_thread));
    if (!workers || !threads) goto done;

    for (i = 0; i < ncpu; i++) {
        workers[i].doc = doc;
        workers[i].ctx = ctx;
        workers[i].npages = npages;
        workers[i].nops = nops;
        workers[i].rng = 0x9e3779b9u ^ (uint32_t)(i + 1) * 0x85ebca6bu;
        if (hu_thread_start(&threads[i], worker_main, &workers[i]) != 0) {
            fprintf(stderr, "cannot create thread %d\n", i);
            started_all = 0;
            break;
        }
    }
    /* Join every thread that started before doc/ctx are torn down below. */
    hu_threads_join(threads, ncpu);
    if (!started_all) goto done;

    errors = hu_atomic_load(&g_errors);
    printf("%s: %d threads, %d ops, %ld errors\n", path, ncpu, nops, errors);
    rc = errors == 0 ? 0 : 1;

done:
    free(threads);
    free(workers);
    if (doc) djvu_doc_close(doc);
    if (ctx) djvu_ctx_free(ctx);
    free(data);
    return rc;
}
