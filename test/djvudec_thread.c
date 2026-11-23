/* djvudec_thread.c -- concurrent stress test for read-only djvu_doc APIs.
 *
 * Calls djvu_init() once, opens a .djvu, then runs randomized page operations
 * from N worker threads. See thread-safety.md.
 *
 *   djvudec_thread [-cpu N] [-nops N] file.djvu
 *
 * Defaults: cpu = processor_count/2 (min 1), nops = 256.
 * Each op picks a random page and one of: render, text, zones, links. */
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
        "usage: %s [-cpu N] [-nops N] file.djvu\n"
        "  stress-test concurrent render/text/zones/links on one open document\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int ncpu = 0, nops = 256, npages, i, started_all = 1, rc = 1;
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
