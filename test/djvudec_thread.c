/* djvudec_thread.c -- concurrent stress test for read-only djvu_doc APIs.
 *
 * Calls djvu_init() once, opens a .djvu, then runs randomized page operations
 * from N worker threads. See thread-safety.md.
 *
 *   djvudec_thread [-cpu N] [-nops N] file.djvu
 *
 * Defaults: cpu = processor_count/2 (min 1), nops = 256.
 * Each op picks a random page and one of: render, text, zones, links. */
#include "djvu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

enum { OP_RENDER = 0, OP_TEXT = 1, OP_ZONES = 2, OP_LINKS = 3, OP_COUNT = 4 };

typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages;
    int nops;
    int thread_id;
    uint32_t rng;
#if !defined(_WIN32)
    pthread_t tid;
#endif
} worker_arg;

#if defined(_WIN32)
static CRITICAL_SECTION g_cache_lock;

static void cache_lock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    EnterCriticalSection(&g_cache_lock);
}

static void cache_unlock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    LeaveCriticalSection(&g_cache_lock);
}

static volatile LONG g_next_op;
static volatile LONG g_errors;
static LONG g_nops_done;

static void inc_error(void) { InterlockedIncrement(&g_errors); }

static int cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#else
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void cache_lock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    pthread_mutex_lock(&g_cache_lock);
}

static void cache_unlock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    pthread_mutex_unlock(&g_cache_lock);
}

static long g_next_op;
static long g_errors;
static long g_nops_done;

static void inc_error(void) { __sync_fetch_and_add(&g_errors, 1); }

static int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 1u;
    return *state;
}

static int acquire_op(void)
{
#if defined(_WIN32)
    long i = InterlockedIncrement(&g_next_op) - 1;
#else
    long i = __sync_fetch_and_add(&g_next_op, 1);
#endif
    return (int)i;
}

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

#if defined(_WIN32)
static DWORD WINAPI worker_main(void *arg)
#else
static void *worker_main(void *arg)
#endif
{
    worker_arg *w = (worker_arg *)arg;

    for (;;) {
        int idx = acquire_op();
        int page, op;

        if (idx >= w->nops) break;

        page = (int)(xorshift32(&w->rng) % (uint32_t)w->npages);
        op = (int)(xorshift32(&w->rng) % (uint32_t)OP_COUNT);

        if (run_one_op(w, op, page) != 0)
            inc_error();
#if defined(_WIN32)
        InterlockedIncrement(&g_nops_done);
    }
    return 0;
#else
        __sync_fetch_and_add(&g_nops_done, 1);
    }
    return NULL;
#endif
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
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
    int ncpu = 0, nops = 256, i, rc = 1;
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    worker_arg *workers = NULL;
#if defined(_WIN32)
    HANDLE *handles = NULL;
#else
#endif

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
        ncpu = cpu_count() / 2;
        if (ncpu < 1) ncpu = 1;
    }
    if (nops <= 0) nops = 256;

    data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    djvu_init();
#if defined(_WIN32)
    InitializeCriticalSection(&g_cache_lock);
#endif
    ctx = djvu_ctx_new(NULL, NULL, cache_lock_cb, cache_unlock_cb, NULL, NULL);
    if (!ctx) goto done;
    djvu_ctx_set_cache_mode(ctx, DJVU_CACHE_ON_DEMAND);

    doc = djvu_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "djvu_doc_open failed\n");
        goto done;
    }

    workers = (worker_arg *)calloc((size_t)ncpu, sizeof(worker_arg));
    if (!workers) goto done;

#if defined(_WIN32)
    g_next_op = 0;
    g_errors = 0;
    g_nops_done = 0;
    handles = (HANDLE *)calloc((size_t)ncpu, sizeof(HANDLE));
    if (!handles) goto done;

    for (i = 0; i < ncpu; i++) {
        workers[i].doc = doc;
        workers[i].ctx = ctx;
        workers[i].npages = djvu_doc_page_count(doc);
        workers[i].nops = nops;
        workers[i].thread_id = i;
        workers[i].rng = 0x9e3779b9u ^ (uint32_t)(i + 1) * 0x85ebca6bu;
        if (workers[i].npages <= 0) {
            fprintf(stderr, "document has no pages\n");
            goto done;
        }
        handles[i] = CreateThread(NULL, 0, worker_main, &workers[i], 0, NULL);
        if (!handles[i]) {
            fprintf(stderr, "CreateThread failed\n");
            goto done;
        }
    }
    WaitForMultipleObjects((DWORD)ncpu, handles, TRUE, INFINITE);
#else
    g_next_op = 0;
    g_errors = 0;
    g_nops_done = 0;

    for (i = 0; i < ncpu; i++) {
        workers[i].doc = doc;
        workers[i].ctx = ctx;
        workers[i].npages = djvu_doc_page_count(doc);
        workers[i].nops = nops;
        workers[i].thread_id = i;
        workers[i].rng = 0x9e3779b9u ^ (uint32_t)(i + 1) * 0x85ebca6bu;
        if (workers[i].npages <= 0) {
            fprintf(stderr, "document has no pages\n");
            goto done;
        }
        if (pthread_create(&workers[i].tid, NULL, worker_main, &workers[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            goto done;
        }
    }
    for (i = 0; i < ncpu; i++)
        pthread_join(workers[i].tid, NULL);
#endif

    printf("%s: %d threads, %d ops, %ld errors\n",
           path, ncpu, nops,
#if defined(_WIN32)
           (long)g_errors);
    rc = g_errors == 0 ? 0 : 1;
#else
           g_errors);
    rc = g_errors == 0 ? 0 : 1;
#endif

done:
#if defined(_WIN32)
    DeleteCriticalSection(&g_cache_lock);
    if (handles) {
        for (i = 0; i < ncpu; i++)
            if (handles[i]) CloseHandle(handles[i]);
        free(handles);
    }
#endif
    free(workers);
    if (doc) djvu_doc_close(doc);
    if (ctx) djvu_ctx_free(ctx);
    free(data);
    return rc;
}