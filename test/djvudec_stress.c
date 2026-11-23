/* djvudec_stress.c -- render every page of every .djvu under a directory.
 *
 * Opens each file once, renders all pages from N worker threads (per-page
 * caching + lock callbacks, like SumatraPDF's EngineDjvuDec), then closes.
 *
 *   djvudec_stress [-cpu N] <dir>
 *
 * Default thread count: processor_count - 2, at least 2.
 * Prints per-file timing: load / render / close. */
#include "harness_util.h"

#if !defined(_WIN32)
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif

typedef struct {
    double load_ms;
    double render_ms;
    double close_ms;
    int npages;
    int render_errors;
} file_timing;

/* stress_file results */
enum {
    STRESS_OK = 0,
    STRESS_RENDER_ERRORS = 1, /* at least one page failed to render */
    STRESS_NO_PAGES = 2,      /* document opened but has zero pages: a skip */
    STRESS_FAILED = 3         /* unreadable / cannot open / setup failure */
};

static int default_thread_count(void)
{
    int n = hu_cpu_count() - 2;
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

typedef struct {
    djvu_doc *doc;
    djvu_ctx *ctx;
    int npages;
    hu_atomic next_page;
    hu_atomic render_errors;
} render_job;

static void render_worker(void *arg)
{
    render_job *job = (render_job *)arg;
    for (;;) {
        int page = (int)hu_atomic_inc(&job->next_page) - 1;
        djvu_image *img;
        if (page >= job->npages) break;
        img = djvu_page_render(job->doc, page, 1);
        if (!img) {
            hu_atomic_inc(&job->render_errors);
            continue;
        }
        djvu_image_destroy(job->ctx, img);
    }
}

/* Returns a STRESS_* code; on STRESS_FAILED *why names the step that failed
   (the decoder's own diagnostics go to stderr via the error callback). */
static int stress_file(const char *path, int ncpu, file_timing *out, const char **why)
{
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    double t0, t_load, t_render, t_close;
    render_job job;
    hu_thread *threads = NULL;
    int i, started_all = 1, rc = STRESS_FAILED;

    memset(out, 0, sizeof(*out));
    *why = "";
    t0 = hu_now_ms();
    data = hu_read_file(path, &len);
    if (!data) { *why = "cannot read file"; return STRESS_FAILED; }

    ctx = djvu_ctx_new(NULL, NULL, hu_lock_cb, hu_unlock_cb, hu_on_error, NULL);
    if (!ctx) { *why = "djvu_ctx_new failed"; goto done; }
    djvu_ctx_set_cache_per_page(ctx, 1);

    doc = djvu_doc_open(ctx, data, len);
    t_load = hu_now_ms() - t0;
    if (!doc) { *why = "djvu_doc_open failed"; goto done; }

    out->npages = djvu_doc_page_count(doc);
    if (out->npages <= 0) { rc = STRESS_NO_PAGES; goto done; }

    memset(&job, 0, sizeof(job));
    job.doc = doc;
    job.ctx = ctx;
    job.npages = out->npages;

    threads = (hu_thread *)calloc((size_t)ncpu, sizeof(hu_thread));
    if (!threads) { *why = "out of memory"; goto done; }

    t0 = hu_now_ms();
    for (i = 0; i < ncpu; i++) {
        if (hu_thread_start(&threads[i], render_worker, &job) != 0) {
            started_all = 0;
            break;
        }
    }
    /* Always join whatever started before touching doc/ctx: the workers that
       did start are rendering from them. */
    hu_threads_join(threads, ncpu);
    if (!started_all) { *why = "cannot create thread"; goto done; }
    t_render = hu_now_ms() - t0;
    out->render_errors = (int)hu_atomic_load(&job.render_errors);

    t0 = hu_now_ms();
    djvu_doc_close(doc);
    doc = NULL;
    djvu_ctx_free(ctx);
    ctx = NULL;
    free(data);
    data = NULL;
    t_close = hu_now_ms() - t0;

    out->load_ms = t_load;
    out->render_ms = t_render;
    out->close_ms = t_close;
    rc = out->render_errors == 0 ? STRESS_OK : STRESS_RENDER_ERRORS;

done:
    if (doc) djvu_doc_close(doc);
    if (ctx) djvu_ctx_free(ctx);
    free(data);
    free(threads);
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
    int nskipped;
} stress_ctx;

static int stress_one(const char *path, void *user)
{
    stress_ctx *ctx = (stress_ctx *)user;
    file_timing t;
    char loadbuf[32], renderbuf[32], closebuf[32];
    const char *why = "";
    int rc;

    rc = stress_file(path, ctx->ncpu, &t, &why);
    ctx->nfiles++;

    /* Only a document that opens cleanly with zero pages is a skip (e.g. an
       indirect-document index). Unreadable / undecodable files and setup
       failures are failures. */
    if (rc == STRESS_NO_PAGES) {
        printf("%s\n  skipped (no pages)\n", path);
        fflush(stdout);
        ctx->nskipped++;
        return 0;
    }
    if (rc == STRESS_FAILED) {
        printf("%s\n  FAILED: %s\n", path, why);
        fflush(stdout);
        ctx->nfailed++;
        return 1;
    }

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

    if (rc != STRESS_OK) ctx->nfailed++;
    return rc != STRESS_OK;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-cpu N] <dir-or-file> ...\n"
        "  render every page of each .djvu file / every .djvu under each dir\n"
        "  default threads: processor_count - 2, at least 2\n",
        prog);
}

static int path_is_dir(const char *path)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int main(int argc, char **argv)
{
    int ncpu = 0, i, rc = 0, npaths = 0;
    stress_ctx ctx;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-cpu") && i + 1 < argc)
            { ncpu = atoi(argv[++i]); continue; }
        if (argv[i][0] == '-') { usage(argv[0]); return 2; }
        npaths++;
    }

    if (npaths == 0) { usage(argv[0]); return 2; }
    if (ncpu <= 0) ncpu = default_thread_count();

    djvu_init();

    memset(&ctx, 0, sizeof(ctx));
    ctx.ncpu = ncpu;

    printf("stress-test (%d threads)\n", ncpu);
    fflush(stdout);

    for (i = 1; i < argc; i++) {
        int frc;
        if (!strcmp(argv[i], "-cpu")) { i++; continue; }
        frc = path_is_dir(argv[i]) ? walk_dir(argv[i], stress_one, &ctx)
                                   : stress_one(argv[i], &ctx);
        if (frc != 0) rc = frc;
    }

    if (rc < 0) {
        fprintf(stderr, "cannot walk one or more paths\n");
        return 1;
    }

    printf("done: %d file(s), %d failed, %d skipped\n",
           ctx.nfiles, ctx.nfailed, ctx.nskipped);
    return ctx.nfailed > 0 ? 1 : 0;
}