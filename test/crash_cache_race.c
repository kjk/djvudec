/* crash_cache_race.c -- regression test for concurrent render +
 * djvu_doc_drop_page_cache.
 *
 * Mimics SumatraPDF EngineDjvuDec: per-page cache, lock callbacks, multiple
 * render threads, and an LRU-style dropper that frees page layers while other
 * threads may still be mid-render. Cached layers are refcounted: a layer pinned
 * by an in-flight render is only freed after its last release, so dropping a
 * page's cache under a running render must be safe. Before that fix this
 * harness died with an ACCESS_VIOLATION in bm_visit_ink_runs_bytes /
 * compose_stencil_sub; it now has to run for the whole duration and exit 0
 * (best run under ASan, which also catches a use-after-free that happens not
 * to crash).
 *
 *   crash_cache_race [-cpu N] [-secs N] [-sub N] file.djvu
 */
#include "harness_util.h"

/* State shared by every worker. */
typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages;
    int subsample;
    hu_atomic stop; /* set by main, polled by the workers */
    hu_atomic renders;
    hu_atomic drops;
    hu_atomic errors;
} race_shared;

typedef struct {
    race_shared *sh;
    uint32_t rng;
} worker_arg;

/* Renderer: random pages at subsample>1 (compose_stencil_sub path). */
static void render_worker(void *arg)
{
    worker_arg *w = (worker_arg *)arg;
    race_shared *sh = w->sh;
    while (!hu_atomic_load(&sh->stop)) {
        int page = (int)(hu_xorshift32(&w->rng) % (uint32_t)sh->npages);
        int sub = sh->subsample;
        if ((hu_xorshift32(&w->rng) & 3) == 0)
            sub = 1 + (int)(hu_xorshift32(&w->rng) % 4); /* 1..4 */
        {
            djvu_image *img = djvu_page_render(sh->doc, page, sub);
            if (img) {
                djvu_image_destroy(sh->ctx, img);
                hu_atomic_inc(&sh->renders);
            } else {
                hu_atomic_inc(&sh->errors);
            }
        }
        /* Sumatra: after render, drop some other page's cache. */
        if ((hu_xorshift32(&w->rng) & 1) != 0) {
            int drop = (int)(hu_xorshift32(&w->rng) % (uint32_t)sh->npages);
            if (drop != page) {
                djvu_doc_drop_page_cache(sh->doc, drop);
                hu_atomic_inc(&sh->drops);
            }
        }
    }
}

/* Dedicated dropper: hammer LRU-style free of all pages. */
static void drop_worker(void *arg)
{
    worker_arg *w = (worker_arg *)arg;
    race_shared *sh = w->sh;
    int i = 0;
    while (!hu_atomic_load(&sh->stop)) {
        djvu_doc_drop_page_cache(sh->doc, i % sh->npages);
        hu_atomic_inc(&sh->drops);
        i++;
        hu_yield();
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-cpu N] [-secs N] [-sub N] file.djvu\n", argv0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int ncpu = 0, secs = 15, sub = 3, i, started_all = 1, rc = 1;
    uint8_t *data = NULL;
    size_t len = 0;
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    worker_arg *workers = NULL;
    hu_thread *threads = NULL;
    race_shared sh;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-cpu") && i + 1 < argc)
            ncpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-secs") && i + 1 < argc)
            secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sub") && i + 1 < argc)
            sub = atoi(argv[++i]);
        else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else
            path = argv[i];
    }
    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (ncpu <= 0) {
        ncpu = hu_cpu_count();
        if (ncpu < 4) ncpu = 4;
        if (ncpu > 12) ncpu = 12;
    }
    if (secs < 1) secs = 1;
    if (sub < 1) sub = 1;

    data = hu_read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    djvu_init();
    ctx = djvu_ctx_new(NULL, NULL, hu_lock_cb, hu_unlock_cb, hu_on_error, NULL);
    if (!ctx) goto done;
    djvu_ctx_set_cache_per_page(ctx, 1);
    djvu_ctx_set_bgr(ctx, 1);

    doc = djvu_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "djvu_doc_open failed\n");
        goto done;
    }

    memset(&sh, 0, sizeof(sh));
    sh.doc = doc;
    sh.ctx = ctx;
    sh.subsample = sub;
    sh.npages = djvu_doc_page_count(doc);
    if (sh.npages <= 0) {
        fprintf(stderr, "no pages\n");
        goto done;
    }
    printf("opened %s: %d pages, %d render threads + 1 dropper, sub=%d, %ds\n",
           path, sh.npages, ncpu, sub, secs);
    fflush(stdout);

    /* slots 0..ncpu-1 render, slot ncpu is the dropper */
    workers = (worker_arg *)calloc((size_t)ncpu + 1, sizeof(worker_arg));
    threads = (hu_thread *)calloc((size_t)ncpu + 1, sizeof(hu_thread));
    if (!workers || !threads) goto done;

    for (i = 0; i <= ncpu; i++) {
        workers[i].sh = &sh;
        workers[i].rng = 0x9e3779b9u ^ (uint32_t)(i + 1) * 0x85ebca6bu;
        if (hu_thread_start(&threads[i], i < ncpu ? render_worker : drop_worker,
                            &workers[i]) != 0) {
            fprintf(stderr, "cannot create thread %d\n", i);
            started_all = 0;
            break;
        }
    }
    if (started_all)
        hu_sleep_ms((unsigned)secs * 1000u);

    /* Stop and join every thread that started before doc/ctx are torn down. */
    hu_atomic_store(&sh.stop, 1);
    hu_threads_join(threads, ncpu + 1);
    if (!started_all) goto done;

    printf("survived: renders=%ld drops=%ld errors=%ld\n",
           hu_atomic_load(&sh.renders), hu_atomic_load(&sh.drops),
           hu_atomic_load(&sh.errors));
    rc = 0;

done:
    free(threads);
    free(workers);
    if (doc) djvu_doc_close(doc);
    if (ctx) djvu_ctx_free(ctx);
    free(data);
    return rc;
}
