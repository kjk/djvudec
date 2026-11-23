/* harness_util.h -- helpers shared by the C test harnesses in test/.
 *
 * Header-only (every function is `static`): each harness is compiled as a
 * single .c file by the build scripts, so shared code cannot live in its own
 * translation unit. Include it FIRST, before any system header: off Windows it
 * sets the feature-test macros its POSIX calls need (clock_gettime, fseeko).
 *
 * Sections: file I/O, clock / cpu count / rng, size-header allocator,
 * lock callbacks, threads + atomics, djvu.h printing helpers.
 *
 * A harness uses only some of these, so unused-function diagnostics are
 * silenced: MSVC C4505 by pragma (it is reported at end of the translation
 * unit, so the pragma is deliberately not popped), GCC/clang per function. */
#ifndef HARNESS_UTIL_H
#define HARNESS_UTIL_H

#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1 /* glibc: clock_gettime / fseeko / sysconf under -std=c11 */
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64 /* 64-bit off_t for fseeko/ftello */
#endif
#endif

#include "djvu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(disable : 4505) /* unreferenced function with internal linkage */
#endif
#if defined(__GNUC__) || defined(__clang__)
#define HU_UNUSED __attribute__((unused))
#else
#define HU_UNUSED
#endif

/* ---------------------------------------------------------------- file I/O */

/* Read a whole file into a malloc'd buffer (free with free()). Returns NULL on
   any error. 64-bit sizes, so a >= 2 GB file works where `long` is 32-bit
   (Windows). An empty file yields a non-NULL buffer and *out_len == 0. */
HU_UNUSED static uint8_t *hu_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long long n;
    size_t len;

    if (!f) return NULL;
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) goto fail;
    n = _ftelli64(f);
    if (n < 0 || _fseeki64(f, 0, SEEK_SET) != 0) goto fail;
#else
    if (fseeko(f, 0, SEEK_END) != 0) goto fail;
    n = (long long)ftello(f);
    if (n < 0 || fseeko(f, 0, SEEK_SET) != 0) goto fail;
#endif
    if ((unsigned long long)n > (unsigned long long)SIZE_MAX) goto fail;
    len = (size_t)n;
    buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) goto fail;
    if (fread(buf, 1, len, f) != len) goto fail;
    fclose(f);
    *out_len = len;
    return buf;
fail:
    free(buf);
    fclose(f);
    return NULL;
}

/* Switch stdout to binary mode (no-op off Windows) before writing raw bytes. */
HU_UNUSED static void hu_stdout_binary(void)
{
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

/* -------------------------------------------------- clock / cpu count / rng */

/* Monotonic timestamp in milliseconds. */
HU_UNUSED static double hu_now_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

HU_UNUSED static int hu_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

HU_UNUSED static void hu_sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

HU_UNUSED static void hu_yield(void)
{
#if defined(_WIN32)
    Sleep(0);
#else
    sched_yield();
#endif
}

/* xorshift32 PRNG; *state must be non-zero (a zero state is re-seeded). */
HU_UNUSED static uint32_t hu_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9e3779b9u;
    return *state;
}

/* ---------------------------------------------------- size-header allocator */

/* malloc/free pair that records each block's size in a 16-byte header (which
   preserves malloc's 16-byte alignment), so a tracking djvu_alloc_cb /
   djvu_free_cb can un-count a block when it is freed. */
#define HU_SIZE_HDR 16

HU_UNUSED static void *hu_size_malloc(size_t size)
{
    uint8_t *p;
    if (size > SIZE_MAX - HU_SIZE_HDR) return NULL;
    p = (uint8_t *)malloc(size + HU_SIZE_HDR);
    if (!p) return NULL;
    memcpy(p, &size, sizeof(size));
    return p + HU_SIZE_HDR;
}

/* Frees a hu_size_malloc block and returns the size it was allocated with
   (0 for NULL). */
HU_UNUSED static size_t hu_size_free(void *ptr)
{
    uint8_t *p;
    size_t size;
    if (!ptr) return 0;
    p = (uint8_t *)ptr - HU_SIZE_HDR;
    memcpy(&size, p, sizeof(size));
    free(p);
    return size;
}

/* ----------------------------------------------------------- lock callbacks */

/* One process-wide mutex exposed as djvu_lock_cb / djvu_unlock_cb (required
   for per-page caching; with them shared JB2 dicts also decode lazily). It is
   initialized on first use (thread-safely) and never torn down, so no init or
   cleanup call is needed. Windows uses a CRITICAL_SECTION, as the harnesses
   always did: an SRWLOCK would need no init, but it starves the cache-dropper
   thread of crash_cache_race.c (about 10x fewer drops in the same time). */
#if defined(_WIN32)
HU_UNUSED static BOOL CALLBACK hu_lock_init_once(PINIT_ONCE once, PVOID param, PVOID *out)
{
    (void)once;
    (void)out;
    InitializeCriticalSection((CRITICAL_SECTION *)param);
    return TRUE;
}

HU_UNUSED static CRITICAL_SECTION *hu_lock_obj(void)
{
    static CRITICAL_SECTION lock;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, hu_lock_init_once, &lock, NULL);
    return &lock;
}

HU_UNUSED static void hu_lock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    EnterCriticalSection(hu_lock_obj());
}

HU_UNUSED static void hu_unlock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    LeaveCriticalSection(hu_lock_obj());
}
#else
HU_UNUSED static pthread_mutex_t *hu_lock_obj(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    return &lock;
}

HU_UNUSED static void hu_lock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    pthread_mutex_lock(hu_lock_obj());
}

HU_UNUSED static void hu_unlock_cb(void *user, void *ctx)
{
    (void)user;
    (void)ctx;
    pthread_mutex_unlock(hu_lock_obj());
}
#endif

/* -------------------------------------------------------- threads + atomics */

typedef void (*hu_thread_fn)(void *arg);

/* A thread slot. Zero-initialize (calloc) an array of these; the slot must not
   move while its thread runs (the thread reads fn/arg from it). */
typedef struct hu_thread {
    hu_thread_fn fn;
    void *arg;
    int started;
#if defined(_WIN32)
    HANDLE h;
#else
    pthread_t tid;
#endif
} hu_thread;

#if defined(_WIN32)
HU_UNUSED static DWORD WINAPI hu_thread_tramp(void *p)
{
    hu_thread *t = (hu_thread *)p;
    t->fn(t->arg);
    return 0;
}
#else
HU_UNUSED static void *hu_thread_tramp(void *p)
{
    hu_thread *t = (hu_thread *)p;
    t->fn(t->arg);
    return NULL;
}
#endif

/* Start fn(arg) on a new thread. Returns 0 on success, -1 on failure (the
   slot then stays "not started", so joining it is a no-op). */
HU_UNUSED static int hu_thread_start(hu_thread *t, hu_thread_fn fn, void *arg)
{
    t->fn = fn;
    t->arg = arg;
    t->started = 0;
#if defined(_WIN32)
    t->h = CreateThread(NULL, 0, hu_thread_tramp, t, 0, NULL);
    if (!t->h) return -1;
#else
    if (pthread_create(&t->tid, NULL, hu_thread_tramp, t) != 0) return -1;
#endif
    t->started = 1;
    return 0;
}

/* Wait for every started thread in t[0..n-1] and release its handle. Joins
   one thread at a time, so it works for any n (WaitForMultipleObjects fails
   above MAXIMUM_WAIT_OBJECTS == 64 handles). Slots that never started are
   skipped, so this is also the cleanup after a partial start. */
HU_UNUSED static void hu_threads_join(hu_thread *t, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!t[i].started) continue;
#if defined(_WIN32)
        if (WaitForSingleObject(t[i].h, INFINITE) != WAIT_OBJECT_0) {
            /* Cannot prove the thread is gone; tearing down shared state under
               it would be a use-after-free, so stop here. */
            fprintf(stderr, "harness: WaitForSingleObject failed (%lu)\n",
                    (unsigned long)GetLastError());
            abort();
        }
        CloseHandle(t[i].h);
        t[i].h = NULL;
#else
        if (pthread_join(t[i].tid, NULL) != 0) {
            fprintf(stderr, "harness: pthread_join failed\n");
            abort();
        }
#endif
        t[i].started = 0;
    }
}

/* Atomic counter / flag shared between threads. */
typedef volatile long hu_atomic;

/* Adds 1; returns the new value. */
HU_UNUSED static long hu_atomic_inc(hu_atomic *p)
{
#if defined(_WIN32)
    return InterlockedIncrement(p);
#else
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#endif
}

HU_UNUSED static long hu_atomic_load(hu_atomic *p)
{
#if defined(_WIN32)
    return InterlockedCompareExchange(p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}

HU_UNUSED static void hu_atomic_store(hu_atomic *p, long v)
{
#if defined(_WIN32)
    InterlockedExchange(p, v);
#else
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
#endif
}

/* -------------------------------------------------- djvu.h printing helpers */

/* Error callback: prints the decoder's diagnostics to stderr. */
HU_UNUSED static void hu_on_error(void *user, djvu_severity sev, const char *msg)
{
    const char *s = sev >= DJVU_SEVERITY_ERROR ? "error" :
                    sev == DJVU_SEVERITY_WARNING ? "warning" : "info";
    (void)user;
    fprintf(stderr, "djvu %s: %s\n", s, msg);
}

HU_UNUSED static const char *hu_page_type_name(djvu_page_type t)
{
    static const char *names[] = {"unknown", "bitonal", "photo", "compound"};
    return names[(t >= 0 && t <= 3) ? t : 0];
}

/* Write img as a PNM (P5 gray / P6 color) to path, or to stdout when path is
   "-". PNM color order is RGB; when bgr != 0 the image data is B,G,R (rendered
   with djvu_ctx_set_bgr) and each pixel is swapped back to RGB on the way out
   so the file is still a correct PPM. Returns 0 on success, -1 on error. */
HU_UNUSED static int hu_write_pnm(const char *path, const djvu_image *img, int bgr)
{
    FILE *f;
    int y, rc = 0;
    size_t rowbytes = (size_t)img->width * (size_t)img->format;

    if (path && !strcmp(path, "-")) {
        hu_stdout_binary();
        f = stdout;
    } else {
        f = fopen(path, "wb");
        if (!f) return -1;
    }
    fprintf(f, "%s\n%d %d\n255\n", img->format == DJVU_FORMAT_RGB24 ? "P6" : "P5",
            img->width, img->height);
    if (bgr && img->format == DJVU_FORMAT_RGB24) {
        uint8_t *row = (uint8_t *)malloc(rowbytes ? rowbytes : 1);
        if (!row) {
            rc = -1;
        } else {
            for (y = 0; y < img->height && rc == 0; y++) {
                const uint8_t *src = img->data + (size_t)y * (size_t)img->stride;
                int x;
                for (x = 0; x < img->width; x++) {
                    row[x * 3 + 0] = src[x * 3 + 2];
                    row[x * 3 + 1] = src[x * 3 + 1];
                    row[x * 3 + 2] = src[x * 3 + 0];
                }
                if (fwrite(row, 1, rowbytes, f) != rowbytes) rc = -1;
            }
            free(row);
        }
    } else {
        for (y = 0; y < img->height && rc == 0; y++)
            if (fwrite(img->data + (size_t)y * (size_t)img->stride, 1, rowbytes, f) !=
                rowbytes)
                rc = -1;
    }
    if (f == stdout) {
        if (fflush(f) != 0) rc = -1;
    } else if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

/* Indented dump of a hidden-text zone tree. */
HU_UNUSED static void hu_print_zone(const djvu_text_zone *z, int depth)
{
    static const char *tn[8] = {"?", "page", "col", "region", "para", "line", "word", "char"};
    int i;
    int t = (z->type >= 1 && z->type <= 7) ? z->type : 0;
    for (i = 0; i < depth; i++) fputs("  ", stdout);
    printf("%s [%d,%d %dx%d]", tn[t], z->x, z->y, z->w, z->h);
    if (z->type == DJVU_ZONE_WORD && z->text) printf(" \"%s\"", z->text);
    putchar('\n');
    for (i = 0; i < z->nchildren; i++) hu_print_zone(&z->children[i], depth + 1);
}

/* Indented dump of the outline; pass depth -1 for the synthetic root. */
HU_UNUSED static void hu_print_outline(const djvu_outline_item *it, int depth)
{
    int i;
    if (it->title) {
        for (i = 0; i < depth; i++) fputs("  ", stdout);
        printf("%s -> %s (page %d)\n", it->title, it->url ? it->url : "", it->page_no);
    }
    for (i = 0; i < it->nchildren; i++) hu_print_outline(&it->children[i], depth + 1);
}

/* One line per hyperlink, or "(no links)". */
HU_UNUSED static void hu_print_links(const djvu_page_links *L)
{
    int k;
    if (!L) {
        printf("(no links)\n");
        return;
    }
    for (k = 0; k < L->nlinks; k++) {
        const djvu_link *e = &L->links[k];
        printf("link %d: [%d,%d %dx%d] shape=%d -> %s%s%s\n", k, e->x, e->y, e->w, e->h,
               e->shape, e->url, e->comment ? " ; " : "", e->comment ? e->comment : "");
    }
}

#endif /* HARNESS_UTIL_H */
