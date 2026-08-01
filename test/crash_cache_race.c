/* crash_cache_race.c -- repro for concurrent render + djvu_doc_drop_page_cache.
 *
 * Mimics SumatraPDF EngineDjvuDec: per-page cache, lock callbacks, multiple
 * render threads, and an LRU-style dropper that frees page layers while other
 * threads may still be mid-render. Expected without a pin/refcount fix:
 * ACCESS_VIOLATION in bm_visit_ink_runs_bytes / compose_stencil_sub.
 *
 *   crash_cache_race [-cpu N] [-secs N] [-sub N] file.djvu
 */
#include "djvu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#endif

typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages;
    int subsample;
    int stop;
    int thread_id;
    uint32_t rng;
    volatile long *renders;
    volatile long *drops;
    volatile long *errors;
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

static void atomic_add(volatile long *p, long v) { InterlockedAdd(p, v); }
static long atomic_load(volatile long *p) { return InterlockedCompareExchange(p, 0, 0); }
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

static void atomic_add(volatile long *p, long v) { __sync_fetch_and_add(p, v); }
static long atomic_load(volatile long *p) { return __sync_fetch_and_add(p, 0); }
#endif

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0xdeadbeefu;
    return *state;
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* Renderer: random pages at subsample>1 (compose_stencil_sub path). */
#if defined(_WIN32)
static DWORD WINAPI render_worker(void *arg)
#else
static void *render_worker(void *arg)
#endif
{
    worker_arg *w = (worker_arg *)arg;
    while (!w->stop) {
        int page = (int)(xorshift32(&w->rng) % (uint32_t)w->npages);
        int sub = w->subsample;
        if ((xorshift32(&w->rng) & 3) == 0)
            sub = 1 + (int)(xorshift32(&w->rng) % 4); /* 1..4 */
        {
            djvu_image *img = djvu_page_render(w->doc, page, sub);
            if (img) {
                djvu_image_destroy(w->ctx, img);
                atomic_add(w->renders, 1);
            } else {
                atomic_add(w->errors, 1);
            }
        }
        /* Sumatra: after render, drop some other page's cache. */
        if ((xorshift32(&w->rng) & 1) != 0) {
            int drop = (int)(xorshift32(&w->rng) % (uint32_t)w->npages);
            if (drop != page) {
                djvu_doc_drop_page_cache(w->doc, drop);
                atomic_add(w->drops, 1);
            }
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* Dedicated dropper: hammer LRU-style free of all pages. */
#if defined(_WIN32)
static DWORD WINAPI drop_worker(void *arg)
#else
static void *drop_worker(void *arg)
#endif
{
    worker_arg *w = (worker_arg *)arg;
    int i = 0;
    while (!w->stop) {
        djvu_doc_drop_page_cache(w->doc, i % w->npages);
        atomic_add(w->drops, 1);
        i++;
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-cpu N] [-secs N] [-sub N] file.djvu\n", argv0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int ncpu = 0, secs = 15, sub = 3, i, rc = 1;
    uint8_t *data = NULL;
    size_t len = 0;
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    worker_arg *workers = NULL;
    volatile long renders = 0, drops = 0, errors = 0;
#if defined(_WIN32)
    HANDLE *handles = NULL;
#else
    /* tids stored in workers */
#endif

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
        ncpu = cpu_count();
        if (ncpu < 4) ncpu = 4;
        if (ncpu > 12) ncpu = 12;
    }
    if (secs < 1) secs = 1;
    if (sub < 1) sub = 1;

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
    djvu_ctx_set_cache_per_page(ctx, 1);
    djvu_ctx_set_bgr(ctx, 1);

    doc = djvu_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "djvu_doc_open failed\n");
        goto done;
    }
    {
        int npages = djvu_doc_page_count(doc);
        if (npages <= 0) {
            fprintf(stderr, "no pages\n");
            goto done;
        }
        printf("opened %s: %d pages, %d render threads + 1 dropper, sub=%d, %ds\n",
               path, npages, ncpu, sub, secs);

        workers = (worker_arg *)calloc((size_t)(ncpu + 1), sizeof(worker_arg));
        if (!workers) goto done;

#if defined(_WIN32)
        handles = (HANDLE *)calloc((size_t)(ncpu + 1), sizeof(HANDLE));
        if (!handles) goto done;
#endif

        for (i = 0; i < ncpu; i++) {
            workers[i].doc = doc;
            workers[i].ctx = ctx;
            workers[i].npages = npages;
            workers[i].subsample = sub;
            workers[i].stop = 0;
            workers[i].thread_id = i;
            workers[i].rng = 0x9e3779b9u ^ (uint32_t)(i + 1) * 0x85ebca6bu;
            workers[i].renders = &renders;
            workers[i].drops = &drops;
            workers[i].errors = &errors;
#if defined(_WIN32)
            handles[i] = CreateThread(NULL, 0, render_worker, &workers[i], 0, NULL);
            if (!handles[i]) goto done;
#else
            if (pthread_create(&workers[i].tid, NULL, render_worker, &workers[i]) != 0)
                goto done;
#endif
        }
        /* dropper */
        workers[ncpu].doc = doc;
        workers[ncpu].ctx = ctx;
        workers[ncpu].npages = npages;
        workers[ncpu].stop = 0;
        workers[ncpu].thread_id = ncpu;
        workers[ncpu].rng = 0xC0FFEEu;
        workers[ncpu].renders = &renders;
        workers[ncpu].drops = &drops;
        workers[ncpu].errors = &errors;
#if defined(_WIN32)
        handles[ncpu] = CreateThread(NULL, 0, drop_worker, &workers[ncpu], 0, NULL);
        if (!handles[ncpu]) goto done;
        Sleep((DWORD)secs * 1000u);
#else
        if (pthread_create(&workers[ncpu].tid, NULL, drop_worker, &workers[ncpu]) != 0)
            goto done;
        {
            struct timespec ts;
            ts.tv_sec = secs;
            ts.tv_nsec = 0;
            nanosleep(&ts, NULL);
        }
#endif
        for (i = 0; i <= ncpu; i++)
            workers[i].stop = 1;
#if defined(_WIN32)
        WaitForMultipleObjects((DWORD)(ncpu + 1), handles, TRUE, INFINITE);
#else
        for (i = 0; i <= ncpu; i++)
            pthread_join(workers[i].tid, NULL);
#endif
        printf("survived: renders=%ld drops=%ld errors=%ld\n",
               atomic_load(&renders), atomic_load(&drops), atomic_load(&errors));
        rc = 0;
    }

done:
#if defined(_WIN32)
    DeleteCriticalSection(&g_cache_lock);
    if (handles) {
        for (i = 0; i <= ncpu; i++)
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
