/* djvudec_stress.c -- render every page of every .djvu under a directory.
 *
 * Opens each file once, renders all pages from N worker threads (per-page
 * caching + lock callbacks, like SumatraPDF's EngineDjvuDec), then closes.
 *
 *   djvudec_stress [-cpu N] <dir>
 *
 * Default thread count: processor_count - 2, at least 2.
 * Prints per-file timing: load / render / close. */
#include "djvu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    double load_ms;
    double render_ms;
    double close_ms;
    int npages;
    int render_errors;
} file_timing;

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

static int cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

static double now_ms(void)
{
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart * 1000.0 / (double)freq.QuadPart;
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

static int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

static int default_thread_count(void)
{
    int n = cpu_count() - 2;
    if (n < 2) n = 2;
    return n;
}

static void format_duration_ms(double ms, char *buf, size_t bufsz)
{
    if (ms < 0.0) ms = 0.0;
    if (ms < 1000.0) {
        snprintf(buf, bufsz, "%.0f ms", ms);
        return;
    }
    if (ms < 60000.0) {
        snprintf(buf, bufsz, "%.2f s", ms / 1000.0);
        return;
    }
    {
        int sec = (int)(ms / 1000.0);
        int min = sec / 60;
        int rem = sec % 60;
        snprintf(buf, bufsz, "%dm %ds", min, rem);
    }
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

typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages;
#if defined(_WIN32)
    volatile LONG next_page;
    volatile LONG render_errors;
#else
    volatile int next_page;
    volatile int render_errors;
#endif
} render_job;

#if defined(_WIN32)
static DWORD WINAPI render_worker(void *arg)
#else
static void *render_worker(void *arg)
#endif
{
    render_job *job = (render_job *)arg;
    for (;;) {
        int page;
#if defined(_WIN32)
        page = (int)InterlockedIncrement(&job->next_page) - 1;
#else
        page = __sync_fetch_and_add(&job->next_page, 1);
#endif
        djvu_image *img;
        if (page >= job->npages) break;
        img = djvu_page_render(job->doc, page, 1);
        if (!img) {
#if defined(_WIN32)
            InterlockedIncrement(&job->render_errors);
#else
            __sync_fetch_and_add(&job->render_errors, 1);
#endif
            continue;
        }
        djvu_image_destroy(job->ctx, img);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int stress_file(const char *path, int ncpu, file_timing *out)
{
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    double t0, t_load, t_render, t_close;
    render_job job;
    int i, rc = -1;
#if defined(_WIN32)
    HANDLE *handles = NULL;
#else
    pthread_t *tids = NULL;
#endif

    memset(out, 0, sizeof(*out));
    t0 = now_ms();
    data = read_file(path, &len);
    if (!data) return -1;

    ctx = djvu_ctx_new(NULL, NULL, cache_lock_cb, cache_unlock_cb, NULL, NULL);
    if (!ctx) goto done;
    djvu_ctx_set_cache_per_page(ctx, 1);

    doc = djvu_doc_open(ctx, data, len);
    t_load = now_ms() - t0;
    if (!doc) goto done;

    out->npages = djvu_doc_page_count(doc);
    if (out->npages <= 0) goto done;

    memset(&job, 0, sizeof(job));
    job.doc = doc;
    job.ctx = ctx;
    job.npages = out->npages;
    job.next_page = 0;
    job.render_errors = 0;

    t0 = now_ms();
#if defined(_WIN32)
    handles = (HANDLE *)calloc((size_t)ncpu, sizeof(HANDLE));
    if (!handles) goto done;
    for (i = 0; i < ncpu; i++) {
        handles[i] = CreateThread(NULL, 0, render_worker, &job, 0, NULL);
        if (!handles[i]) goto done;
    }
    WaitForMultipleObjects((DWORD)ncpu, handles, TRUE, INFINITE);
#else
    tids = (pthread_t *)calloc((size_t)ncpu, sizeof(pthread_t));
    if (!tids) goto done;
    for (i = 0; i < ncpu; i++) {
        if (pthread_create(&tids[i], NULL, render_worker, &job) != 0) goto done;
    }
    for (i = 0; i < ncpu; i++)
        pthread_join(tids[i], NULL);
#endif
    t_render = now_ms() - t0;
    out->render_errors = job.render_errors;

    t0 = now_ms();
    djvu_doc_close(doc);
    doc = NULL;
    djvu_ctx_free(ctx);
    ctx = NULL;
    free(data);
    data = NULL;
    t_close = now_ms() - t0;

    out->load_ms = t_load;
    out->render_ms = t_render;
    out->close_ms = t_close;
    rc = out->render_errors == 0 ? 0 : 1;

done:
    if (doc) djvu_doc_close(doc);
    if (ctx) djvu_ctx_free(ctx);
    free(data);
#if defined(_WIN32)
    if (handles) {
        for (i = 0; i < ncpu; i++)
            if (handles[i]) CloseHandle(handles[i]);
        free(handles);
    }
#else
    free(tids);
#endif
    return rc;
}

typedef int (*path_fn)(const char *path, void *user);

#if defined(_WIN32)
static int walk_dir(const char *dir, path_fn fn, void *user)
{
    char pattern[4096];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t dirlen, patlen;
    int rc = 0;

    dirlen = strlen(dir);
    if (dirlen + 4 >= sizeof(pattern)) return -1;
    memcpy(pattern, dir, dirlen);
    patlen = dirlen;
    if (patlen > 0 && pattern[patlen - 1] != '\\' && pattern[patlen - 1] != '/') {
        pattern[patlen++] = '\\';
    }
    memcpy(pattern + patlen, "*", 2);

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    do {
        char child[4096];
        const char *name = fd.cFileName;
        size_t nlen, pos;

        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        nlen = strlen(name);
        if (dirlen + nlen + 2 >= sizeof(child)) continue;
        memcpy(child, dir, dirlen);
        pos = dirlen;
        if (pos > 0 && child[pos - 1] != '\\' && child[pos - 1] != '/')
            child[pos++] = '\\';
        memcpy(child + pos, name, nlen + 1);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int r = walk_dir(child, fn, user);
            if (r < 0) rc = -1;
            else if (r > 0 && rc == 0) rc = 1;
        } else {
            size_t clen = strlen(child);
            if (clen > 4) {
                const char *ext = child + clen - 4;
                if (!_stricmp(ext, ".djv")) {
                    if (fn(child, user) != 0 && rc == 0) rc = 1;
                } else if (clen > 5 && !_stricmp(child + clen - 5, ".djvu")) {
                    if (fn(child, user) != 0 && rc == 0) rc = 1;
                }
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return rc;
}
#else
static int walk_dir(const char *dir, path_fn fn, void *user)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    int rc = 0;

    if (!d) return -1;
    while ((ent = readdir(d)) != NULL) {
        char child[4096];
        struct stat st;
        const char *name = ent->d_name;
        size_t dirlen, nlen;

        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        dirlen = strlen(dir);
        nlen = strlen(name);
        if (dirlen + 1 + nlen + 1 >= sizeof(child)) continue;
        snprintf(child, sizeof(child), "%s/%s", dir, name);
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int r = walk_dir(child, fn, user);
            if (r < 0) rc = -1;
            else if (r > 0 && rc == 0) rc = 1;
        } else {
            size_t clen = strlen(child);
            if (clen > 4) {
                if (!strcasecmp(child + clen - 4, ".djv")) {
                    if (fn(child, user) != 0 && rc == 0) rc = 1;
                } else if (clen > 5 && !strcasecmp(child + clen - 5, ".djvu")) {
                    if (fn(child, user) != 0 && rc == 0) rc = 1;
                }
            }
        }
    }
    closedir(d);
    return rc;
}
#endif

typedef struct {
    int ncpu;
    int nfiles;
    int nfailed;
} stress_ctx;

static int stress_one(const char *path, void *user)
{
    stress_ctx *ctx = (stress_ctx *)user;
    file_timing t;
    char loadbuf[32], renderbuf[32], closebuf[32];
    int rc;

    rc = stress_file(path, ctx->ncpu, &t);
    format_duration_ms(t.load_ms, loadbuf, sizeof(loadbuf));
    format_duration_ms(t.render_ms, renderbuf, sizeof(renderbuf));
    format_duration_ms(t.close_ms, closebuf, sizeof(closebuf));

    printf("%s\n", path);
    printf("  load %s, render %d pages in %s (%d threads), close %s",
           loadbuf, t.npages, renderbuf, ctx->ncpu, closebuf);
    if (t.render_errors > 0)
        printf(", %d render error(s)", t.render_errors);
    printf("\n");
    fflush(stdout);

    ctx->nfiles++;
    if (rc != 0) ctx->nfailed++;
    return rc;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-cpu N] <dir>\n"
        "  render every page of every .djvu/.djv under <dir>\n"
        "  default threads: processor_count - 2, at least 2\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    int ncpu = 0, i, rc;
    stress_ctx ctx;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-cpu") && i + 1 < argc)
            ncpu = atoi(argv[++i]);
        else if (argv[i][0] == '-')
            { usage(argv[0]); return 2; }
        else
            dir = argv[i];
    }

    if (!dir) { usage(argv[0]); return 2; }
    if (ncpu <= 0) ncpu = default_thread_count();

    djvu_init();
#if defined(_WIN32)
    InitializeCriticalSection(&g_cache_lock);
#endif

    memset(&ctx, 0, sizeof(ctx));
    ctx.ncpu = ncpu;

    printf("stress-test %s (%d threads)\n", dir, ncpu);
    fflush(stdout);

    rc = walk_dir(dir, stress_one, &ctx);

#if defined(_WIN32)
    DeleteCriticalSection(&g_cache_lock);
#endif

    if (rc < 0) {
        fprintf(stderr, "cannot walk %s\n", dir);
        return 1;
    }

    printf("done: %d file(s), %d failed\n", ctx.nfiles, ctx.nfailed);
    return ctx.nfailed > 0 ? 1 : 0;
}