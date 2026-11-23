/* djvu_test.c -- CLI harness for the djvu library (jbig2dec-style tool).
 *
 *   djvu_test -info <in.djvu>
 *   djvu_test -page <N> -out <out.pgm|out.ppm> <in.djvu>   (1-based page)
 *   djvu_test -page <N> -text <in.djvu>
 *
 * Used to verify against DjVuLibre's ddjvu / djvutxt. Run with no arguments
 * for the full flag list (usage() below). */
#include "harness_util.h"
#include "djvu_internal.h"
#include <signal.h>
#include <stdarg.h>
#if defined(_WIN32)
#include <direct.h>
#include <malloc.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/stat.h>
#endif

/* timing / oracle helpers from bench_ddjvu.cpp (DjVuLibre, same clock) */
typedef struct bench_render {
    int width, height;
    int bps;
    int rowsize;
    uint8_t *data;
} bench_render;

double bench_now_ms(void);
typedef struct bench_session_timings {
    double open_ms;
    double close_ms;
    double total_ms;
    double *page_ms;
    int npages;
} bench_session_timings;

int bench_ddjvu_session(const char *path, int sum, bench_session_timings *out);
int bench_ddjvu_render_page(const char *path, int page0, int want_rgb,
                            bench_render *out);
void bench_render_free(bench_render *img);
void bench_ddjvu_reset(void);
void bench_ddjvu_purge(void);
void bench_ddjvu_mem_debug(FILE *out);

/* ---- per-context memory accounting (djvu_alloc_cb / djvu_free_cb hooks) ----
   Installed by -verify-render so each file's run reports total allocations and
   peak live memory, confirms every allocation was freed, and aborts (exit 3) if
   a single djvu_ctx's live memory exceeds 4 GB. hu_size_malloc records each
   block's size so frees can be attributed. The djvu_ctx arg lets us keep
   separate tallies per context. Single-threaded (one thread per process). */
#define MEM_LIMIT_BYTES ((size_t)4 * 1024 * 1024 * 1024)
#define MEM_MAX_CTX 64

typedef struct {
    void *ctx;
    size_t total;        /* cumulative bytes ever allocated */
    size_t live;         /* currently live bytes */
    size_t peak;         /* peak live bytes */
    long n_alloc, n_free;
} mem_rec;

static mem_rec g_mem[MEM_MAX_CTX];
static int g_mem_n;

static mem_rec *mem_find(void *ctx)
{
    int i;
    for (i = 0; i < g_mem_n; i++)
        if (g_mem[i].ctx == ctx) return &g_mem[i];
    if (g_mem_n < MEM_MAX_CTX) {
        mem_rec *r = &g_mem[g_mem_n++];
        memset(r, 0, sizeof(*r));
        r->ctx = ctx;
        return r;
    }
    return NULL; /* table full (won't happen: tests use one ctx per process) */
}

/* "1.50 GB" / "234.00 MB" / "512.00 KB" / "42 B"; buf must hold >= 32 bytes. */
static const char *human_bytes(size_t bytes, char *buf)
{
    double b = (double)bytes;
    const char *u = "B";
    if (b >= 1024.0) { b /= 1024.0; u = "KB"; }
    if (b >= 1024.0) { b /= 1024.0; u = "MB"; }
    if (b >= 1024.0) { b /= 1024.0; u = "GB"; }
    snprintf(buf, 32, u[0] == 'B' ? "%.0f %s" : "%.2f %s", b, u);
    return buf;
}

static void *mem_alloc(void *user, void *ctx, size_t size)
{
    void *p;
    mem_rec *r;
    (void)user;
    p = hu_size_malloc(size);
    if (!p) return NULL;
    r = mem_find(ctx);
    if (r) {
        r->total += size;
        r->live += size;
        if (r->live > r->peak) r->peak = r->live;
        r->n_alloc++;
        /* abort the moment a single context's live memory passes 4 GB */
        if (ctx && r->live > MEM_LIMIT_BYTES) {
            char a[32], b[32];
            fprintf(stderr, "mem\tLIMIT\tctx=%p\tlive=%s\tlimit=%s\n", ctx,
                    human_bytes(r->live, a), human_bytes(MEM_LIMIT_BYTES, b));
            fflush(stderr);
            exit(3);
        }
    }
    return p;
}

static void mem_free(void *user, void *ctx, void *ptr)
{
    size_t size;
    mem_rec *r;
    (void)user;
    if (!ptr) return;
    size = hu_size_free(ptr);
    r = mem_find(ctx);
    if (r) { r->live -= size; r->n_free++; }
}

/* After a file: human-readable per-context report (stderr) + a machine-readable
   aggregate line (stdout, parsed by cmd/tests.ts). Returns a nonzero code if any
   context leaked (live != 0, or alloc/free counts differ), else rc unchanged. */
static int mem_finish(int rc)
{
    size_t total = 0, peak = 0, live = 0;
    long na = 0, nf = 0;
    int i, leak = 0;
    char tb[32], pb[32];

    for (i = 0; i < g_mem_n; i++) {
        mem_rec *r = &g_mem[i];
        fprintf(stderr,
                "mem\tctx=%p\ttotal=%s\tpeak=%s\tallocs=%ld\tfrees=%ld\tlive=%zu\n",
                r->ctx, human_bytes(r->total, tb), human_bytes(r->peak, pb),
                r->n_alloc, r->n_free, r->live);
        total += r->total;
        if (r->peak > peak) peak = r->peak; /* contexts are sequential -> max */
        live += r->live;
        na += r->n_alloc;
        nf += r->n_free;
        if (r->live != 0 || r->n_alloc != r->n_free) leak = 1;
    }
    /* tab-separated for cmd/tests.ts: memstat <total> <peak> <allocs> <frees>
       <live> (bytes). Distinct kind so it can't collide with the RSS "mem"
       checkpoint lines. */
    printf("memstat\t%zu\t%zu\t%ld\t%ld\t%zu\n", total, peak, na, nf, live);
    fflush(stdout);
    if (leak) {
        fprintf(stderr, "mem\tLEAK\tlive=%zu\tallocs=%ld\tfrees=%ld\n", live, na, nf);
        fflush(stderr);
        return rc ? rc : 4;
    }
    return rc;
}

/* tests.ts textNorm: strip CR/FF and trailing whitespace. */
static char *text_normalize(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *d = (char *)malloc(n + 1);
    size_t i, w = 0;
    if (!d) return NULL;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r' || c == '\f') continue;
        d[w++] = c;
    }
    while (w > 0) {
        char c = d[w - 1];
        if (c != ' ' && c != '\t' && c != '\n') break;
        w--;
    }
    d[w] = 0;
    return d;
}

static char *read_all_stdin(size_t *out_len)
{
    size_t cap = 65536, n = 0;
    char *buf = (char *)malloc(cap);
    size_t r;
    if (!buf) return NULL;
    while ((r = fread(buf + n, 1, cap - n - 1, stdin)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) return NULL;
        }
    }
    buf[n] = 0;
    *out_len = n;
    return buf;
}

/* tests.ts packs per-page djvutxt --page=N blobs: u32-BE length + bytes per page. */
static char **read_ref_text_pages(const char *blob, size_t len, int npages)
{
    char **pages;
    size_t pos = 0;
    int p;
    if (npages <= 0) return NULL;
    pages = (char **)calloc((size_t)npages, sizeof(char *));
    if (!pages) return NULL;
    for (p = 0; p < npages; p++) {
        uint32_t slen;
        char *slice;
        if (pos + 4 > len) break;
        slen = ((uint32_t)(unsigned char)blob[pos] << 24) |
               ((uint32_t)(unsigned char)blob[pos + 1] << 16) |
               ((uint32_t)(unsigned char)blob[pos + 2] << 8) |
               (uint32_t)(unsigned char)blob[pos + 3];
        pos += 4;
        if (pos + slen > len) break;
        slice = (char *)malloc((size_t)slen + 1);
        if (slice) {
            if (slen) memcpy(slice, blob + pos, slen);
            slice[slen] = 0;
            pages[p] = text_normalize(slice);
            free(slice);
        }
        pos += slen;
    }
    for (; p < npages; p++)
        pages[p] = text_normalize("");
    return pages;
}

typedef enum { PK_OTHER, PK_MASK, PK_BG } page_kind_t;

static page_kind_t page_kind(djvu_doc *doc, int page0)
{
    uint32_t form_off = doc->pages[page0].form_off;
    uint32_t sz;
    int has_sjbz = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL) != NULL;
    /* BG44, or standalone PM44/BM44 (libdjvu image/x-iw44), or FG/JPEG. */
    int has_bg = djvu_form_bg_iw44_id(doc, form_off) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "FG44", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "BGjp", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "FGjp", &sz, NULL) != NULL;
    if (has_bg) return PK_BG;
    if (has_sjbz) return PK_MASK;
    return PK_OTHER;
}

static double bench_best2(double a, double b)
{
    if (a < 0.0) return b;
    if (b < 0.0) return a;
    return a < b ? a : b;
}

typedef struct {
    char op[16];
    double ours;
    double lib;
} bench_cmp_row;

static void bench_fmt_ms_cell(char *buf, size_t cap, double ms)
{
    if (ms < 0.0)
        snprintf(buf, cap, "ERROR");
    else
        snprintf(buf, cap, "%.2f", ms);
}

static void bench_fmt_diff_cell(char *buf, size_t cap, double ours, double lib)
{
    if (ours < 0.0 || lib < 0.0)
        snprintf(buf, cap, "ERROR");
    else
        snprintf(buf, cap, "%+.2f", ours - lib);
}

static void bench_fmt_pct_cell(char *buf, size_t cap, double ours, double lib)
{
    if (ours < 0.0 || lib < 0.0)
        snprintf(buf, cap, "ERROR");
    else if (lib > 0.0)
        snprintf(buf, cap, "%+.1f%%", (ours - lib) / lib * 100.0);
    else
        snprintf(buf, cap, "0.0%%");
}

static void bench_print_compare_table(const bench_cmp_row *rows, int nrows)
{
    static const char *h_op = "op";
    static const char *h_ours = "djvudec";
    static const char *h_lib = "libdjvu";
    static const char *h_diff = "diff";
    static const char *h_pct = "%diff";
    int w_op = (int)strlen(h_op);
    int w_ours = (int)strlen(h_ours);
    int w_lib = (int)strlen(h_lib);
    int w_diff = (int)strlen(h_diff);
    int w_pct = (int)strlen(h_pct);
    char ours[24], lib[24], diff[24], pct[24];
    int i, w;

    for (i = 0; i < nrows; i++) {
        bench_fmt_ms_cell(ours, sizeof ours, rows[i].ours);
        bench_fmt_ms_cell(lib, sizeof lib, rows[i].lib);
        bench_fmt_diff_cell(diff, sizeof diff, rows[i].ours, rows[i].lib);
        bench_fmt_pct_cell(pct, sizeof pct, rows[i].ours, rows[i].lib);
        w = (int)strlen(rows[i].op);
        if (w > w_op) w_op = w;
        w = (int)strlen(ours);
        if (w > w_ours) w_ours = w;
        w = (int)strlen(lib);
        if (w > w_lib) w_lib = w;
        w = (int)strlen(diff);
        if (w > w_diff) w_diff = w;
        w = (int)strlen(pct);
        if (w > w_pct) w_pct = w;
    }

    printf("%-*s %-*s %-*s %-*s %-*s\n",
           w_op, h_op, w_lib, h_lib, w_ours, h_ours, w_diff, h_diff, w_pct, h_pct);
    for (i = 0; i < nrows; i++) {
        bench_fmt_ms_cell(ours, sizeof ours, rows[i].ours);
        bench_fmt_ms_cell(lib, sizeof lib, rows[i].lib);
        bench_fmt_diff_cell(diff, sizeof diff, rows[i].ours, rows[i].lib);
        bench_fmt_pct_cell(pct, sizeof pct, rows[i].ours, rows[i].lib);
        printf("%-*s %-*s %-*s %-*s %-*s\n",
               w_op, rows[i].op, w_lib, lib, w_ours, ours, w_diff, diff, w_pct, pct);
    }
}

static int sum_page_subsample(djvu_doc *doc, int page0);
static int sum_render_into(djvu_doc *doc, int page0, int subsample, uint8_t *dst);
static uint8_t *sum_dst_alloc(djvu_doc *doc, int page0, int subsample);

/* Open doc, render each page, close; record open/page/close times.
   The per-page clock is kept symmetric with bench_ddjvu_session
   (bench_ddjvu.cpp):
     sum == 0: djvu_page_render allocates its result inside the timer (libdjvu's
               side mallocs its output inside the timer too); the image is
               destroyed after the clock stops, on both sides.
     sum != 0: the destination buffer (the engine's DIB stand-in) is allocated
               and touched before the clock starts and freed after it stops, on
               both sides; the subsample pick is engine load-time work and is
               outside the timer as well.
   total_ms is the sum of the timed regions (open + pages + close) on both
   sides. */
static int bench_ours_session(djvu_ctx *ctx, const uint8_t *data, size_t len, int sum,
                              bench_session_timings *out)
{
    djvu_doc *doc;
    int n, i;
    double t0;

    if (!out || !out->page_ms || out->npages <= 0)
        return -1;

    t0 = bench_now_ms();
    doc = djvu_doc_open(ctx, data, len);
    out->open_ms = bench_now_ms() - t0;
    if (!doc)
        return -1;

    n = djvu_doc_page_count(doc);
    if (n > out->npages)
        n = out->npages;

    for (i = 0; i < n; i++) {
        if (sum) {
            int subsample = sum_page_subsample(doc, i);
            uint8_t *dst = sum_dst_alloc(doc, i, subsample);
            int ok;

            if (!dst) {
                out->page_ms[i] = -1.0;
                continue;
            }
            t0 = bench_now_ms();
            ok = sum_render_into(doc, i, subsample, dst) == 0;
            out->page_ms[i] = ok ? bench_now_ms() - t0 : -1.0;
            free(dst);
        } else {
            djvu_image *img;

            t0 = bench_now_ms();
            img = djvu_page_render(doc, i, 1);
            out->page_ms[i] = bench_now_ms() - t0;
            if (img)
                djvu_image_destroy(ctx, img);
            else
                out->page_ms[i] = -1.0;
        }
    }
    for (i = n; i < out->npages; i++)
        out->page_ms[i] = -1.0;

    t0 = bench_now_ms();
    djvu_doc_close(doc);
    out->close_ms = bench_now_ms() - t0;
    /* total = the timed regions only (open + rendered pages + close), so the
       untimed destination-buffer handling above cannot leak into it */
    out->total_ms = out->open_ms + out->close_ms;
    for (i = 0; i < n; i++)
        if (out->page_ms[i] >= 0.0)
            out->total_ms += out->page_ms[i];
    return 0;
}

static djvu_ctx *bench_ctx_new(void)
{
    /* Always supply the lock hooks: shared JB2 dicts then decode lazily on
       first use (like libdjvu) instead of eagerly at open, so -bench charges
       dict decode to the page that needs it on both sides. */
    return djvu_ctx_new(NULL, NULL, hu_lock_cb, hu_unlock_cb, NULL, NULL);
}

static void bench_ctx_configure(djvu_ctx *ctx, int per_page, int sum)
{
    djvu_ctx_set_cache_per_page(ctx, per_page);
    if (sum)
        djvu_ctx_set_bgr(ctx, 1);
}

/* --- bench-sum: replicate SumatraPDF EngineDjvuDec::RenderPage (our path) ---
 * Renders at zoom=1, user-rotation=0. Mirrors the engine's non-GDI work: pick
 * an integer subsample, query the output geometry, then render straight into a
 * destination buffer (djvu_page_render_into -- the engine's DIB), so there is
 * no separate format-convert/copy pass. The decoder now applies intrinsic page
 * rotation itself (at every subsample, via a tiled transpose), so there is no
 * engine-side rotate either. The GDI StretchBlt/DIB allocation is excluded by
 * design (the dst buffer stands in for the DIB; sum_dst_alloc allocates and
 * touches it outside the timer). */

static volatile uint8_t sum_sink; /* defeat dead-code elimination of the render */

static int sum_normalize_rotation(int r)
{
    r = r % 360;
    if (r < 0)
        r += 360;
    return r;
}

/* EngineDjvuDec::DjvuDecPickSubsample: largest subsample whose decoded bitmap
   still covers the target pixel size (so StretchBlt only shrinks, never
   upscales). The coverage test is ceil(dim/(s+1)) >= target; plain floor
   division (dim/target) is off by one when target doesn't divide dim -- e.g.
   3597/1799 == 1 even though ceil(3597/2) == 1799 still covers 1799, which left
   high-dpi pages rendering at full resolution (4x the pixels) at zoom=1.
   Compound (color) pages subsample too now that the library composes color at
   any subsample. */
static int sum_pick_subsample(int upW, int upH, int tdx, int tdy)
{
    int s;
    if (upW <= 0 || upH <= 0 || tdx <= 0 || tdy <= 0)
        return 1;
    s = 1;
    while ((upW + s) / (s + 1) >= tdx && (upH + s) / (s + 1) >= tdy)
        s++; /* ceil(upW/(s+1)) >= tdx && ceil(upH/(s+1)) >= tdy */
    if (s > upW)
        s = upW;
    if (s > upH)
        s = upH;
    return s;
}

/* Compute the engine's subsample for a page (cached at engine load time, so
   done outside the render timer here). Intrinsic rotation is now applied by the
   decoder at every subsample, so the engine no longer rotates separately. */
static int sum_page_subsample(djvu_doc *doc, int page0)
{
    djvu_page_info info;
    int dpi, rot, upW, upH, fullW, fullH;
    memset(&info, 0, sizeof info);
    if (djvu_doc_page_info(doc, page0, &info) != 0)
        return 1;
    dpi = info.dpi;
    if (dpi < 25 || dpi > 6000)
        dpi = 300;
    rot = sum_normalize_rotation(info.rotation);
    upW = info.width;
    upH = info.height;
    if (rot == 90 || rot == 270) {
        int t = upW;
        upW = upH;
        upH = t;
    }
    fullW = (int)(upW * 300.0 / dpi + 0.5);
    fullH = (int)(upH * 300.0 / dpi + 0.5);
    return sum_pick_subsample(upW, upH, fullW, fullH);
}

/* Render a page the way the updated EngineDjvuDec does: query geometry, then
   render straight into dst (no convert pass; the decoder also applies intrinsic
   rotation). dst comes from sum_dst_alloc for the same page + subsample. */
static int sum_render_into(djvu_doc *doc, int page0, int subsample, uint8_t *dst)
{
    djvu_render_info ri;
    int comp, stride;
    if (djvu_page_render_info(doc, page0, subsample, &ri) != 0)
        return -1;
    comp = (ri.format == DJVU_FORMAT_GRAY8) ? 1 : 3;
    stride = ri.width * comp;
    if (djvu_page_render_into(doc, page0, subsample, dst, stride) != 0)
        return -1;
    sum_sink ^= dst[0];
    return 0;
}

/* Allocate the destination buffer for a sum render of this page at this
   subsample (exact size from djvu_page_render_info) and touch every byte, so
   neither the allocation nor its first-touch page faults land in a timed
   region. Returns NULL if the page cannot be rendered. Free with free(). */
static uint8_t *sum_dst_alloc(djvu_doc *doc, int page0, int subsample)
{
    djvu_render_info ri;
    size_t n;
    uint8_t *dst;
    if (djvu_page_render_info(doc, page0, subsample, &ri) != 0 ||
        ri.width <= 0 || ri.height <= 0)
        return NULL;
    n = (size_t)ri.width * (size_t)ri.height *
        (size_t)(ri.format == DJVU_FORMAT_GRAY8 ? 1 : 3);
    dst = (uint8_t *)malloc(n);
    if (dst) memset(dst, 0xff, n);
    return dst;
}

/* Tight loop for macOS `sample` profiling: one doc open, repeated sum renders. */
static int profile_sum_page(djvu_ctx *ctx, const uint8_t *data, size_t len, int page0)
{
    djvu_doc *doc;
    int subsample, n;
    uint8_t *dst;

    doc = djvu_doc_open(ctx, data, len);
    if (!doc)
        return 1;
    n = djvu_doc_page_count(doc);
    if (page0 < 0 || page0 >= n) {
        fprintf(stderr, "profile-sum: page %d out of range (1..%d)\n", page0 + 1, n);
        djvu_doc_close(doc);
        return 1;
    }
    subsample = sum_page_subsample(doc, page0);
    dst = sum_dst_alloc(doc, page0, subsample);
    if (!dst) {
        djvu_doc_close(doc);
        return 1;
    }
    fprintf(stderr, "profile-sum: ready page=%d subsample=%d\n", page0 + 1, subsample);
    fflush(stderr);
    for (;;)
        sum_render_into(doc, page0, subsample, dst);
    /* not reached */
}

/* One untimed, tracked document pass (open + render all pages + text + links +
   close) using the accounting allocator, then print the decoder's allocation
   stats for the whole document: count, total bytes, peak live bytes. Kept out of
   the timed bench loops so the header/accounting overhead doesn't skew timings.
   `sum` selects the EngineDjvuDec render path (bench-sum) vs plain render. */
static void bench_mem_report(const uint8_t *data, size_t len, int sum)
{
    djvu_ctx *ctx;
    djvu_doc *doc;
    size_t total = 0, peak = 0;
    long na = 0;
    char tb[32], pb[32];
    int i;

    g_mem_n = 0; /* fresh accounting for this pass */
    ctx = djvu_ctx_new(mem_alloc, mem_free, NULL, NULL, hu_on_error, NULL);
    if (!ctx) return;
    if (sum) djvu_ctx_set_bgr(ctx, 1);
    doc = djvu_doc_open(ctx, data, len);
    if (doc) {
        int n = djvu_doc_page_count(doc);
        for (i = 0; i < n; i++) {
            if (sum) {
                int ss = sum_page_subsample(doc, i);
                uint8_t *dst = sum_dst_alloc(doc, i, ss);
                if (dst) { sum_render_into(doc, i, ss, dst); free(dst); }
            } else {
                djvu_image *img = djvu_page_render(doc, i, 1);
                if (img) djvu_image_destroy(ctx, img);
            }
            { char *t = djvu_page_text(doc, i); if (t) djvu_text_destroy(ctx, t); }
            { djvu_page_links *L = djvu_page_get_links(doc, i);
              if (L) djvu_page_links_destroy(ctx, L); }
        }
        djvu_doc_close(doc);
    }
    djvu_ctx_free(ctx);

    for (i = 0; i < g_mem_n; i++) {
        total += g_mem[i].total;
        if (g_mem[i].peak > peak) peak = g_mem[i].peak;
        na += g_mem[i].n_alloc;
    }
    printf("document, allocs %ld, total %s, peak %s\n",
           na, human_bytes(total, tb), human_bytes(peak, pb));
}

/* Verify djvu_page_render_into is byte-identical to djvu_page_render (+ matching
   djvu_page_render_info geometry) for every page at subsamples 1, 2 and 3 (the
   reduced subsamples also exercise the subsampled color composite). */
static int run_verify_into(djvu_doc *doc)
{
    djvu_ctx *ctx = doc->ctx;
    int n = djvu_doc_page_count(doc), i, si, mism = 0, checked = 0;
    static const int subs[] = { 1, 2, 3 };
    for (i = 0; i < n; i++) {
        for (si = 0; si < (int)(sizeof(subs) / sizeof(subs[0])); si++) {
            int sub = subs[si];
            djvu_render_info ri;
            djvu_image *img;
            uint8_t *dst;
            int comp, stride, y, bad = 0;
            if (djvu_page_render_info(doc, i, sub, &ri) != 0)
                continue; /* page renders nothing at this subsample */
            img = djvu_page_render(doc, i, sub);
            if (!img)
                continue;
            if (img->width != ri.width || img->height != ri.height ||
                img->format != ri.format) {
                printf("page %d sub %d: render_info geometry mismatch\n", i + 1, sub);
                mism++; djvu_image_destroy(ctx, img); continue;
            }
            comp = (ri.format == DJVU_FORMAT_GRAY8) ? 1 : 3;
            stride = ri.width * comp;
            dst = (uint8_t *)malloc((size_t)stride * ri.height);
            if (!dst) { djvu_image_destroy(ctx, img); continue; }
            checked++;
            if (djvu_page_render_into(doc, i, sub, dst, stride) != 0) {
                printf("page %d sub %d: render_into failed\n", i + 1, sub); mism++;
            } else {
                for (y = 0; y < ri.height && !bad; y++)
                    if (memcmp(dst + (size_t)y * stride,
                               img->data + (size_t)y * img->stride, (size_t)stride) != 0)
                        bad = 1;
                if (bad) { printf("page %d sub %d: render_into bytes differ\n", i + 1, sub); mism++; }
            }
            free(dst);
            djvu_image_destroy(ctx, img);
        }
    }
    printf("verify-into: %d/%d page renders checked, %d mismatch\n", checked, n * 3, mism);
    return mism ? 1 : 0;
}

/* ======================================================================
 * -verify-abort: deterministic, single-threaded test of the abort API
 * (djvu_request_abort, djvu_abort tokens, the *_abortable render variants).
 *
 * Abort is cooperative: the decoder polls djvu_aborted() at a few checkpoints
 * (jb2.c once per decoded record; render.c after the JB2 mask is acquired and
 * every 64 blits of the bitonal loops; compose.c at composite entry and every 64 blits of the stencil loop;
 * iw44.c once per decoded slice and around each plane's inverse transform).
 * So what an abort request may do depends on the page:
 *   - bitonal / compound / photo: a request made before the call, or early
 *     in the decode, MUST abort the render;
 *   - blank / INFO-only (type unknown): no checkpoint, always completes.
 * In every case the invariant is: a render returns either "aborted" (NULL / -1)
 * or output byte-identical to an undisturbed render -- never a partial image --
 * and an aborted render leaks nothing and leaves no stale state behind.
 *
 * The in-flight case is made deterministic by aborting from the allocation
 * callback when the render's allocation count reaches N, sweeping N
 * geometrically (x2 at subsample 1, x4 at subsample 3, to bound the run time)
 * from 1 up to the number of allocations a full render makes (pages without
 * an in-flight checkpoint only get the first and the last allocation).
 * ====================================================================== */

#define VA_MAX_PAGES 4 /* pages sampled when -page is not given */

enum { VA_TOKEN = 0, VA_CTX = 1 };   /* how the allocator requests the abort */
enum { VA_RENDER = 0, VA_INTO = 1 }; /* which render entry point */

/* Counting allocator state (the ctx's `user` pointer). */
typedef struct {
    long n_live;  /* outstanding blocks */
    size_t live;  /* outstanding bytes */
    long count;   /* allocations since the trigger was armed */
    long trig_at; /* request the abort when count reaches this (0 = never) */
    int trig_mode;
    int fired;
    djvu_abort *tok;
} va_mem;

static void *va_alloc(void *user, void *ctx, size_t size)
{
    va_mem *m = (va_mem *)user;
    void *p;
    m->count++;
    if (m->trig_at && m->count == m->trig_at) {
        m->fired = 1;
        if (m->trig_mode == VA_TOKEN)
            djvu_abort_request(m->tok);
        else if (ctx)
            djvu_request_abort((djvu_ctx *)ctx); /* aborts all in-flight renders */
    }
    p = hu_size_malloc(size);
    if (p) {
        m->n_live++;
        m->live += size;
    }
    return p;
}

static void va_free(void *user, void *ctx, void *ptr)
{
    va_mem *m = (va_mem *)user;
    (void)ctx;
    if (!ptr) return;
    m->n_live--;
    m->live -= hu_size_free(ptr);
}

static void va_arm(va_mem *m, long at, int mode, djvu_abort *tok)
{
    m->count = 0;
    m->fired = 0;
    m->trig_mode = mode;
    m->tok = tok;
    m->trig_at = at;
}

/* Reference pixels of an undisturbed render (rows packed, no padding). */
typedef struct {
    int w, h;
    djvu_format fmt;
    size_t n; /* w * h * bytes-per-pixel */
    uint8_t *px;
} va_ref;

/* One context under test: per-page caching off (no lock hooks: shared dicts
   decode eagerly at open) or on (lock hooks: shared dicts decode lazily). */
typedef struct {
    const char *name;
    int cached;
    djvu_ctx *ctx;
    djvu_doc *doc;
    va_mem mem;
} va_side;

typedef struct {
    long checks, failures, aborted;
    int page1, sub;     /* current position, for messages */
    const char *side;
} va_stats;

/* Count one check; print a FAIL line when cond is false. Returns cond. */
static int va_check(va_stats *st, int cond, const char *fmt, ...)
{
    st->checks++;
    if (!cond) {
        va_list ap;
        st->failures++;
        printf("verify-abort: FAIL page %d sub %d (%s): ", st->page1, st->sub, st->side);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
        fflush(stdout);
    }
    return cond;
}

/* Copy img's pixels into *r and destroy img. Returns 0 on success. */
static int va_ref_take(djvu_ctx *ctx, djvu_image *img, va_ref *r)
{
    size_t rowb = (size_t)img->width * (size_t)img->format;
    int y;
    r->w = img->width;
    r->h = img->height;
    r->fmt = img->format;
    r->n = rowb * (size_t)img->height;
    r->px = (uint8_t *)malloc(r->n ? r->n : 1);
    if (r->px)
        for (y = 0; y < img->height; y++)
            memcpy(r->px + (size_t)y * rowb, img->data + (size_t)y * (size_t)img->stride, rowb);
    djvu_image_destroy(ctx, img);
    return r->px ? 0 : -1;
}

static int va_img_eq(const djvu_image *img, const va_ref *r)
{
    size_t rowb = (size_t)r->w * (size_t)r->fmt;
    int y;
    if (img->width != r->w || img->height != r->h || img->format != r->fmt) return 0;
    for (y = 0; y < r->h; y++)
        if (memcmp(img->data + (size_t)y * (size_t)img->stride, r->px + (size_t)y * rowb,
                   rowb) != 0)
            return 0;
    return 1;
}

/* One render of `page` through entry point `api`: the *_abortable variant with
   `tok` when abortable != 0, else the plain variant. Returns -1 if it returned
   "aborted / failed" (NULL or -1), 1 if it produced output byte-identical to
   ref, 0 if it produced different output. dst is scratch of ref->n bytes. */
static int va_render(djvu_ctx *ctx, djvu_doc *doc, int page, int sub, int api,
                     int abortable, const djvu_abort *tok, const va_ref *ref,
                     uint8_t *dst)
{
    if (api == VA_RENDER) {
        djvu_image *img = abortable ? djvu_page_render_abortable(doc, page, sub, tok)
                                    : djvu_page_render(doc, page, sub);
        int eq;
        if (!img) return -1;
        eq = va_img_eq(img, ref);
        djvu_image_destroy(ctx, img);
        return eq;
    } else {
        int stride = ref->w * (int)ref->fmt;
        int rc;
        memset(dst, 0xA5, ref->n);
        rc = abortable ? djvu_page_render_into_abortable(doc, page, sub, dst, stride, tok)
                       : djvu_page_render_into(doc, page, sub, dst, stride);
        if (rc != 0) return -1;
        return memcmp(dst, ref->px, ref->n) == 0;
    }
}

static const char *va_api_name(int api, int abortable)
{
    if (api == VA_RENDER)
        return abortable ? "djvu_page_render_abortable" : "djvu_page_render";
    return abortable ? "djvu_page_render_into_abortable" : "djvu_page_render_into";
}

/* (d) in-flight abort sweep on the already-open document. base_n / base_b is
   the allocator's live state with nothing of this page cached. */
static void va_sweep(va_side *s, int page, int sub, const va_ref *ref, uint8_t *dst,
                     long nalloc, int inflight, long base_n, size_t base_b, va_stats *st)
{
    long n, aborted = 0;
    int last = 0, mode, api;
    int step = sub == 1 ? 2 : 4;

    if (nalloc <= 0) {
        if (inflight)
            va_check(st, 0, "a full render made no allocations; cannot sweep");
        return;
    }
    for (n = 1; !last; n *= step) {
        /* A page with no in-flight checkpoint always completes, at full render
           cost: just probe the first and the last allocation. */
        if (n >= nalloc || (!inflight && n > 1)) {
            n = nalloc;
            last = 1;
        }
        for (mode = VA_TOKEN; mode <= VA_CTX; mode++) {
            for (api = VA_RENDER; api <= VA_INTO; api++) {
                const char *via = mode == VA_TOKEN ? "token" : "djvu_request_abort";
                const char *fn = va_api_name(api, mode == VA_TOKEN);
                djvu_abort tok;
                int r;

                djvu_abort_init(&tok);
                va_arm(&s->mem, n, mode, &tok);
                r = va_render(s->ctx, s->doc, page, sub, api, mode == VA_TOKEN, &tok, ref, dst);
                va_arm(&s->mem, 0, mode, NULL);
                if (r < 0) {
                    int r2;
                    aborted++;
                    st->aborted++;
                    if (!s->cached)
                        va_check(st, s->mem.n_live == base_n && s->mem.live == base_b,
                                 "%s aborted at alloc %ld via %s leaked: %ld blocks / %zu "
                                 "bytes live, expected %ld / %zu",
                                 fn, n, via, s->mem.n_live, s->mem.live, base_n, base_b);
                    /* proves no stale abort state (TLS token / epoch) and an
                       intact page cache */
                    r2 = va_render(s->ctx, s->doc, page, sub, VA_RENDER, 0, NULL, ref, dst);
                    va_check(st, r2 == 1,
                             "plain render after %s aborted at alloc %ld via %s %s",
                             fn, n, via, r2 < 0 ? "failed" : "differs from the reference");
                } else {
                    va_check(st, r == 1,
                             "%s with abort at alloc %ld via %s returned a partial / "
                             "different image", fn, n, via);
                }
                djvu_doc_drop_page_cache(s->doc, page);
                va_check(st, s->mem.n_live == base_n && s->mem.live == base_b,
                         "after %s with abort at alloc %ld via %s (+ cache drop): %ld blocks "
                         "/ %zu bytes live, expected %ld / %zu",
                         fn, n, via, s->mem.n_live, s->mem.live, base_n, base_b);
            }
        }
    }
    if (inflight)
        va_check(st, aborted > 0,
                 "no render in the in-flight sweep (allocs 1..%ld) was aborted", nalloc);
}

/* (e) cold-document sweep, lock-hook context only: open a fresh document for
   every N so the abort can land inside the lazy shared-dictionary decode and
   the first-time page-cache fill, then render again undisturbed on the same
   document, close it and require the allocator to be back where it started. */
static void va_cold_sweep(va_side *s, const uint8_t *data, size_t len, int page, int sub,
                          const va_ref *ref, uint8_t *dst, int inflight, va_stats *st)
{
    long base_n = s->mem.n_live, n, nalloc = 0;
    size_t base_b = s->mem.live;
    int last = 0;
    int step = sub == 1 ? 2 : 4;

    for (n = 0; !last; n = n ? n * step : 1) {
        djvu_doc *doc2;
        djvu_abort tok;
        int r;

        if (n > 0 && (n >= nalloc || (!inflight && n > 1))) {
            n = nalloc;
            last = 1;
        }
        doc2 = djvu_doc_open(s->ctx, data, len);
        if (!va_check(st, doc2 != NULL, "cold sweep: djvu_doc_open failed"))
            return;
        djvu_abort_init(&tok);
        va_arm(&s->mem, n, VA_TOKEN, &tok); /* n == 0: undisturbed control run */
        r = va_render(s->ctx, doc2, page, sub, VA_RENDER, 1, &tok, ref, dst);
        if (n == 0) {
            nalloc = s->mem.count;
            if (nalloc <= 0) last = 1;
        }
        va_arm(&s->mem, 0, VA_TOKEN, NULL);
        if (r < 0) {
            int r2;
            if (va_check(st, n > 0, "cold sweep: undisturbed render on a fresh document failed"))
                st->aborted++;
            r2 = va_render(s->ctx, doc2, page, sub, VA_RENDER, 0, NULL, ref, dst);
            va_check(st, r2 == 1,
                     "cold sweep: plain render after an abort at alloc %ld %s", n,
                     r2 < 0 ? "failed" : "differs from the reference");
        } else {
            va_check(st, r == 1,
                     "cold sweep: abort at alloc %ld returned a partial / different image", n);
        }
        djvu_doc_close(doc2);
        va_check(st, s->mem.n_live == base_n && s->mem.live == base_b,
                 "cold sweep: after abort at alloc %ld + djvu_doc_close: %ld blocks / %zu "
                 "bytes live, expected %ld / %zu",
                 n, s->mem.n_live, s->mem.live, base_n, base_b);
    }
}

/* All abort checks for one page at one subsample on one context. Returns 1 if
   the page was tested, 0 if its plain render fails (nothing to compare). */
static int va_page(va_side *s, const uint8_t *data, size_t len, int page, int sub,
                   va_stats *st)
{
    djvu_page_type type = djvu_page_get_type(s->doc, page);
    /* see the checkpoint list at the top of this section */
    int must_abort = (type != DJVU_PAGE_UNKNOWN);
    int inflight = (type != DJVU_PAGE_UNKNOWN);
    djvu_image *img;
    djvu_render_info ri;
    djvu_abort tok;
    va_ref ref;
    uint8_t *dst;
    long base_n, nalloc;
    size_t base_b;
    int r, api;

    st->page1 = page + 1;
    st->sub = sub;
    st->side = s->name;

    img = djvu_page_render(s->doc, page, sub);
    if (!img) return 0;
    if (va_ref_take(s->ctx, img, &ref) != 0) {
        va_check(st, 0, "out of memory");
        return 0;
    }
    dst = (uint8_t *)malloc(ref.n ? ref.n : 1);
    if (!dst) {
        va_check(st, 0, "out of memory");
        free(ref.px);
        return 0;
    }
    djvu_doc_drop_page_cache(s->doc, page);
    base_n = s->mem.n_live;
    base_b = s->mem.live;

    /* control: an undisturbed re-render is identical, memory-neutral, and tells
       us how many allocations a full render makes */
    va_arm(&s->mem, 0, VA_TOKEN, NULL);
    r = va_render(s->ctx, s->doc, page, sub, VA_RENDER, 0, NULL, &ref, dst);
    nalloc = s->mem.count;
    va_check(st, r == 1, "second plain render %s", r < 0 ? "failed" : "differs from the first");
    djvu_doc_drop_page_cache(s->doc, page);
    va_check(st, s->mem.n_live == base_n && s->mem.live == base_b,
             "plain render is not memory-neutral: %ld blocks / %zu bytes live, expected "
             "%ld / %zu", s->mem.n_live, s->mem.live, base_n, base_b);
    va_check(st, djvu_page_render_info(s->doc, page, sub, &ri) == 0 && ri.width == ref.w &&
                     ri.height == ref.h && ri.format == ref.fmt,
             "djvu_page_render_info disagrees with the render");

    for (api = VA_RENDER; api <= VA_INTO; api++) {
        /* (b) a fresh token and a NULL token behave like the plain variants */
        djvu_abort_init(&tok);
        r = va_render(s->ctx, s->doc, page, sub, api, 1, &tok, &ref, dst);
        va_check(st, r == 1, "%s with a fresh token %s", va_api_name(api, 1),
                 r < 0 ? "failed" : "differs from the plain render");
        r = va_render(s->ctx, s->doc, page, sub, api, 1, NULL, &ref, dst);
        va_check(st, r == 1, "%s with a NULL token %s", va_api_name(api, 1),
                 r < 0 ? "failed" : "differs from the plain render");

        /* (a) a token requested before the call aborts the render; the request
           is sticky, so a second call with the same token aborts too */
        djvu_abort_request(&tok);
        r = va_render(s->ctx, s->doc, page, sub, api, 1, &tok, &ref, dst);
        if (must_abort) {
            if (va_check(st, r < 0, "%s with an already-requested token did not abort",
                         va_api_name(api, 1)))
                st->aborted++;
            r = va_render(s->ctx, s->doc, page, sub, api, 1, &tok, &ref, dst);
            va_check(st, r < 0, "%s: abort request is not sticky", va_api_name(api, 1));
        } else {
            va_check(st, r != 0, "%s with an already-requested token returned a partial / "
                                 "different image", va_api_name(api, 1));
        }
        /* ... and leaves nothing behind for the next plain call */
        r = va_render(s->ctx, s->doc, page, sub, api, 0, NULL, &ref, dst);
        va_check(st, r == 1, "%s after an aborted call %s", va_api_name(api, 0),
                 r < 0 ? "failed" : "differs from the reference");

        /* (c) djvu_request_abort with no render in flight does not affect the
           next render */
        djvu_request_abort(s->ctx);
        r = va_render(s->ctx, s->doc, page, sub, api, 0, NULL, &ref, dst);
        va_check(st, r == 1, "%s after an idle djvu_request_abort %s", va_api_name(api, 0),
                 r < 0 ? "failed" : "differs from the reference");
    }
    djvu_doc_drop_page_cache(s->doc, page);
    va_check(st, s->mem.n_live == base_n && s->mem.live == base_b,
             "token / idle-abort checks leaked: %ld blocks / %zu bytes live, expected "
             "%ld / %zu", s->mem.n_live, s->mem.live, base_n, base_b);

    va_sweep(s, page, sub, &ref, dst, nalloc, inflight, base_n, base_b, st);
    if (s->cached)
        va_cold_sweep(s, data, len, page, sub, &ref, dst, inflight, st);

    free(dst);
    free(ref.px);
    return 1;
}

/* Debug setters, on separate contexts (they must be set before djvu_doc_open):
   djvu_ctx_set_iw_max_chunks(ctx, 1) still renders a color page at the same
   geometry; djvu_ctx_set_no_compose(ctx, 1) makes a compound page render as
   the gray mask, a photo page not render at all, and djvu_page_render_info
   agree with what the render actually does. `plain` is an ordinary document. */
static void va_debug_setters(const uint8_t *data, size_t len, djvu_doc *plain,
                             const int *pages, const int *ref_ok, int npick, va_stats *st)
{
    int which, k;

    st->sub = 1;
    for (which = 0; which < 2; which++) {
        djvu_ctx *ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, NULL, NULL);
        djvu_doc *doc;

        st->side = which == 0 ? "iw_max_chunks=1" : "no_compose=1";
        st->page1 = 0;
        if (!va_check(st, ctx != NULL, "djvu_ctx_new failed")) return;
        if (which == 0) djvu_ctx_set_iw_max_chunks(ctx, 1);
        else djvu_ctx_set_no_compose(ctx, 1);
        doc = djvu_doc_open(ctx, data, len);
        if (!va_check(st, doc != NULL, "djvu_doc_open failed")) {
            djvu_ctx_free(ctx);
            return;
        }
        for (k = 0; k < npick; k++) {
            int page = pages[k];
            djvu_page_type type = djvu_page_get_type(plain, page);
            djvu_render_info rp, ri;
            djvu_image *img;
            int have_ri;

            if (!ref_ok[k] || djvu_page_render_info(plain, page, 1, &rp) != 0) continue;
            st->page1 = page + 1;
            have_ri = djvu_page_render_info(doc, page, 1, &ri) == 0;
            img = djvu_page_render(doc, page, 1);
            if (which == 0) {
                if (rp.format != DJVU_FORMAT_RGB24) {
                    if (img) djvu_image_destroy(ctx, img);
                    continue; /* setter only concerns IW44 (color) pages */
                }
                va_check(st, img != NULL, "color page failed to render");
                if (img)
                    va_check(st, img->width == rp.width && img->height == rp.height &&
                                     img->format == DJVU_FORMAT_RGB24,
                             "render is %dx%d fmt %d, expected %dx%d RGB24",
                             img->width, img->height, (int)img->format, rp.width, rp.height);
            } else if (type == DJVU_PAGE_PHOTO) {
                va_check(st, img == NULL && !have_ri,
                         "photo page: render %s, render_info %s; expected both to fail",
                         img ? "succeeded" : "failed", have_ri ? "succeeded" : "failed");
            } else {
                if (type == DJVU_PAGE_COMPOUND)
                    va_check(st, img != NULL && img->format == DJVU_FORMAT_GRAY8,
                             "compound page did not render as GRAY8 (%s)",
                             img ? "got RGB24" : "render failed");
                va_check(st, (img != NULL) == have_ri,
                         "render %s but render_info %s", img ? "succeeded" : "failed",
                         have_ri ? "succeeded" : "failed");
                if (img && have_ri)
                    va_check(st, img->width == ri.width && img->height == ri.height &&
                                     img->format == ri.format,
                             "render is %dx%d fmt %d, render_info says %dx%d fmt %d",
                             img->width, img->height, (int)img->format, ri.width, ri.height,
                             (int)ri.format);
            }
            if (img) djvu_image_destroy(ctx, img);
        }
        djvu_doc_close(doc);
        djvu_ctx_free(ctx);
    }
}

/* page1: 1-based page to test, or 0 to sample up to VA_MAX_PAGES pages spread
   evenly over the document. Returns 0 if every check passed, else 1. */
static int run_verify_abort(const uint8_t *data, size_t len, int page1)
{
    static const int subs[] = { 1, 3 };
    va_side side[2];
    va_stats st;
    int pages[VA_MAX_PAGES], ref_ok[VA_MAX_PAGES];
    int npages, npick = 0, tested = 0, i, k, si;

    memset(&st, 0, sizeof(st));
    memset(side, 0, sizeof(side));
    st.side = "setup";
    side[0].name = "cache off";
    side[1].name = "cache on";
    side[1].cached = 1;
    for (i = 0; i < 2; i++) {
        va_side *s = &side[i];
        s->ctx = djvu_ctx_new(va_alloc, va_free, s->cached ? hu_lock_cb : NULL,
                              s->cached ? hu_unlock_cb : NULL, NULL, &s->mem);
        if (s->ctx) {
            djvu_ctx_set_cache_per_page(s->ctx, s->cached);
            s->doc = djvu_doc_open(s->ctx, data, len);
        }
        if (!s->doc) {
            printf("verify-abort: cannot open document (%s)\n", s->name);
            st.failures++;
        }
    }

    if (!st.failures) {
        npages = djvu_doc_page_count(side[0].doc);
        if (page1 > 0) {
            if (page1 <= npages) pages[npick++] = page1 - 1;
            else {
                printf("verify-abort: page %d out of range (1..%d)\n", page1, npages);
                st.failures++;
            }
        } else {
            int want = npages < VA_MAX_PAGES ? npages : VA_MAX_PAGES;
            for (k = 0; k < want; k++)
                pages[npick++] = (int)((long long)k * npages / want);
        }
        for (k = 0; k < npick; k++) {
            int any = 0;
            ref_ok[k] = 0;
            for (si = 0; si < (int)(sizeof(subs) / sizeof(subs[0])); si++) {
                for (i = 0; i < 2; i++) {
                    int ok = va_page(&side[i], data, len, pages[k], subs[si], &st);
                    if (ok) any = 1;
                    if (ok && i == 0 && subs[si] == 1) ref_ok[k] = 1;
                }
            }
            if (any) tested++;
            else printf("verify-abort: page %d: plain render fails, not tested\n", pages[k] + 1);
        }
        va_debug_setters(data, len, side[0].doc, pages, ref_ok, npick, &st);
    }

    /* everything a context ever allocated must be gone once it is freed */
    st.page1 = 0;
    st.sub = 0;
    for (i = 0; i < 2; i++) {
        va_side *s = &side[i];
        st.side = s->name;
        if (s->doc) djvu_doc_close(s->doc);
        if (s->ctx) {
            djvu_ctx_free(s->ctx);
            va_check(&st, s->mem.n_live == 0 && s->mem.live == 0,
                     "leak at djvu_ctx_free: %ld blocks / %zu bytes still live",
                     s->mem.n_live, s->mem.live);
        }
    }
    if (tested == 0 && !st.failures) {
        printf("verify-abort: no page could be tested\n");
        st.failures++;
    }

    printf("verify-abort: %d pages, %ld checks, %ld aborted, %ld failures\n",
           tested, st.checks, st.aborted, st.failures);
    return st.failures ? 1 : 0;
}

/* Result of comparing our render with the oracle's. */
enum { IMG_DIFF = 0, IMG_SAME = 1, IMG_COSMETIC = 2 };

/* Number of differing pixels between two same-size RGB images. */
static size_t image_rgb_diff_pixels(const djvu_image *mine, const bench_render *ref)
{
    size_t diffs = 0;
    int y, x;
    for (y = 0; y < mine->height; y++) {
        const uint8_t *a = mine->data + (size_t)y * (size_t)mine->stride;
        const uint8_t *b = ref->data + (size_t)y * (size_t)ref->rowsize;
        for (x = 0; x < mine->width; x++) {
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) diffs++;
            a += 3;
            b += 3;
        }
    }
    return diffs;
}

/* IMG_SAME: byte-identical. IMG_COSMETIC: an RGB page with a tiny number of
   differing pixels (<= 2000 and <= 0.01% of the page; *ndiff receives the
   count) -- tolerated because of ddjvu's FG44 stencil quirk (mask/bg/fg layers
   are byte-exact; only composite FG placement drifts ~1px on rare text lines).
   The caller reports it distinctly so the tolerance is never silent.
   IMG_DIFF: anything else. */
static int image_compare(const djvu_image *mine, const bench_render *ref, size_t *ndiff)
{
    int y, same = 1;
    size_t row, total, diffs;

    *ndiff = 0;
    if (!mine || !ref || !ref->data) return IMG_DIFF;
    if (mine->width != ref->width || mine->height != ref->height) return IMG_DIFF;
    if ((int)mine->format != ref->bps) return IMG_DIFF;
    row = (size_t)mine->width * (size_t)mine->format;
    for (y = 0; y < mine->height && same; y++)
        if (memcmp(mine->data + (size_t)y * (size_t)mine->stride,
                   ref->data + (size_t)y * (size_t)ref->rowsize, row) != 0)
            same = 0;
    if (same) return IMG_SAME;
    if (mine->format != DJVU_FORMAT_RGB24) return IMG_DIFF;

    total = (size_t)mine->width * (size_t)mine->height;
    diffs = image_rgb_diff_pixels(mine, ref);
    *ndiff = diffs;
    if (diffs > 0 && diffs <= 2000 && diffs * 1000000 / total <= 100) /* <= 0.01% */
        return IMG_COSMETIC;
    return IMG_DIFF;
}

static void ensure_dir(const char *dir)
{
    if (!dir || !dir[0]) return;
#if defined(_WIN32)
    (void)_mkdir(dir);
#else
    (void)mkdir(dir, 0755);
#endif
}

#if defined(_WIN32)
static int verify_mem_counters(size_t *ws_bytes, size_t *commit_bytes)
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (!ws_bytes || !commit_bytes)
        return 0;
    *ws_bytes = *commit_bytes = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        *ws_bytes = (size_t)pmc.WorkingSetSize;
        *commit_bytes = (size_t)pmc.PagefileUsage;
        return 1;
    }
    return 0;
}
#else
static int verify_mem_counters(size_t *ws_bytes, size_t *commit_bytes)
{
    if (ws_bytes) *ws_bytes = 0;
    if (commit_bytes) *commit_bytes = 0;
    return 0;
}
#endif

/* Log RSS/commit + decoder/oracle state; stop when either exceeds DJVU_VERIFY_MEM_MB. */
static int verify_mem_checkpoint(djvu_doc *doc, int page1, const char *stage)
{
    const char *lim_s = getenv("DJVU_VERIFY_MEM_MB");
    size_t limit_mb = lim_s ? (size_t)atoi(lim_s) : 4096;
    size_t ws = 0, commit = 0;
    size_t ws_mb, commit_mb;

    verify_mem_counters(&ws, &commit);
    ws_mb = ws / (1024 * 1024);
    commit_mb = commit / (1024 * 1024);

    if (limit_mb) {
        fprintf(stderr, "mem\t%d\t%s\tws_mb=%zu\tcommit_mb=%zu",
                page1, stage, ws_mb, commit_mb);
        bench_ddjvu_mem_debug(stderr);
        fprintf(stderr, "\n");
        djvu_debug_verify_mem(doc, page1, stage, stderr);
        fflush(stderr);
    }
    if (limit_mb && (ws_mb >= limit_mb || commit_mb >= limit_mb)) {
        fprintf(stderr,
                "mem\t%d\tLIMIT\tws_mb=%zu\tcommit_mb=%zu\tlimit_mb=%zu\n",
                page1, ws_mb, commit_mb, limit_mb);
        fflush(stderr);
        return -1;
    }
    return 0;
}

/* Trim process working set after each verify page (MSVC heap + pipe flush). */
static void verify_page_finish(void)
{
#if defined(_WIN32)
    _heapmin();
#endif
    fflush(stdout);
}

/* Crash breadcrumbs for -verify-render: which page and which side (ours or the
   in-process ddjvu oracle) was running when the process died. tests.ts reads
   the stderr line; without it a CI crash is just "exited 9" (Bun keeps only
   the low byte of 0xC0000409). __fastfail / /GS failures bypass every handler,
   but the page lines on stdout are flushed per page, so the runner still knows
   the last completed page. */
static volatile int g_verify_page;
static const char *volatile g_verify_phase = "startup";

static void verify_crash_note(const char *what, unsigned long code)
{
    fprintf(stderr, "crash\t%d\t%s\t%s\t0x%08lx\n", g_verify_page,
            g_verify_phase, what, code);
    fflush(stderr);
}

static void verify_on_sigabrt(int sig)
{
    (void)sig;
    verify_crash_note("abort", 0);
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DJVU_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define DJVU_TEST_ASAN 1
#endif

#if defined(_WIN32) && !defined(DJVU_TEST_ASAN)
static LONG WINAPI verify_on_seh(EXCEPTION_POINTERS *ep)
{
    verify_crash_note("exception", ep && ep->ExceptionRecord
                                       ? (unsigned long)ep->ExceptionRecord->ExceptionCode
                                       : 0);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if defined(_WIN32)
static void verify_on_invalid_param(const wchar_t *expr, const wchar_t *func,
                                    const wchar_t *file, unsigned line,
                                    uintptr_t reserved)
{
    (void)expr; (void)func; (void)file; (void)line; (void)reserved;
    verify_crash_note("crt-invalid-parameter", 0);
    abort();
}
#endif

static void verify_install_crash_handlers(void)
{
    signal(SIGABRT, verify_on_sigabrt);
#if defined(_WIN32)
#if !defined(DJVU_TEST_ASAN) /* ASan owns the exception filter and reports with the stack */
    SetUnhandledExceptionFilter(verify_on_seh);
#endif
    _set_invalid_parameter_handler(verify_on_invalid_param);
#endif
}

/* In-memory render verify vs ddjvuapi; write PNMs to diffdir only on mismatch. */
static int run_verify_render(djvu_doc *doc, const char *path, const char *diffdir)
{
    djvu_ctx *ctx = doc->ctx;
    int npages = djvu_doc_page_count(doc);
    int m = 0, mm = 0, skip = 0, cosmetic = 0;
    int i;

    /* Compare in BGR: DjVuLibre's pixmap is natively B,G,R, so asking ddjvu for
       BGR24 is a memcpy (RGB24 costs it a per-pixel swap). We render BGR too
       (djvu_ctx_set_bgr); the byte-for-byte memcmp is then RGB-swap-free on the
       reference side. BGR<->BGR equality is exactly as strict as RGB<->RGB. */
    djvu_ctx_set_bgr(ctx, 1);

    {
        const char *lo_s = getenv("DJVU_VERIFY_LO");
        const char *hi_s = getenv("DJVU_VERIFY_HI");
        int lo = lo_s ? atoi(lo_s) : 1;
        int hi = hi_s ? atoi(hi_s) : npages;
        if (lo < 1) lo = 1;
        if (hi > npages) hi = npages;
        ensure_dir(diffdir);
        if (verify_mem_checkpoint(doc, lo, "chunk_start") < 0)
            goto mem_limit;
        for (i = lo - 1; i < hi; i++) {
        page_kind_t kind;
        g_verify_page = i + 1;
        g_verify_phase = "ours";
        kind = page_kind(doc, i);
        if (kind == PK_OTHER) {
            skip++;
            printf("render\t%d\tskip\n", i + 1);
            if (verify_mem_checkpoint(doc, i + 1, "skip") < 0)
                goto mem_limit;
            verify_page_finish();
            continue;
        }
        {
            int want_rgb = (kind == PK_BG);
            const char *ext = want_rgb ? "ppm" : "pgm";
            djvu_image *mine = djvu_page_render(doc, i, 1);
            bench_render ref;
            int cmp = IMG_DIFF;

            memset(&ref, 0, sizeof(ref));
            if (verify_mem_checkpoint(doc, i + 1, "after_ours_render") < 0)
                goto mem_limit;
            if (getenv("DJVU_VERIFY_OURS_ONLY")) {
                if (!mine) {
                    mm++;
                    printf("render\t%d\terror\n", i + 1);
                    verify_page_finish();
                    continue;
                }
                m++;
                printf("render\t%d\tok\n", i + 1);
                djvu_image_destroy(ctx, mine);
                djvu_doc_drop_page_cache(doc, i);
                if (verify_mem_checkpoint(doc, i + 1, "after_page_free") < 0)
                    goto mem_limit;
                verify_page_finish();
                continue;
            }
            g_verify_phase = "ddjvu";
            if (!mine || bench_ddjvu_render_page(path, i, want_rgb, &ref) != 0) {
                if (mine) djvu_image_destroy(ctx, mine);
                bench_render_free(&ref);
                djvu_doc_drop_page_cache(doc, i);
                mm++;
                printf("render\t%d\terror\n", i + 1);
                verify_page_finish();
                continue;
            }
            if (verify_mem_checkpoint(doc, i + 1, "after_ddjvu_render") < 0)
                goto mem_limit;
            g_verify_phase = "compare";
            {
                size_t ndiff = 0;
                cmp = image_compare(mine, &ref, &ndiff);
                if (cmp == IMG_SAME) {
                    m++;
                    printf("render\t%d\tok\n", i + 1);
                } else if (cmp == IMG_COSMETIC) {
                    /* Within the oracle-quirk tolerance (see image_compare):
                       counted as ok, but flagged with two extra fields so the
                       runner can count / show it: "cosmetic" + differing
                       pixel count. */
                    m++;
                    cosmetic++;
                    printf("render\t%d\tok\tcosmetic\t%zu\n", i + 1, ndiff);
                }
            }
            if (cmp == IMG_DIFF) {
                mm++;
                if (diffdir && diffdir[0]) {
                    char refpath[1024], minepath[1024];
                    snprintf(refpath, sizeof(refpath), "%s/p%d_ref.%s",
                             diffdir, i + 1, ext);
                    snprintf(minepath, sizeof(minepath), "%s/p%d_mine.%s",
                             diffdir, i + 1, ext);
                    {
                        djvu_image refimg;
                        refimg.width = ref.width;
                        refimg.height = ref.height;
                        refimg.format = ref.bps == 3 ? DJVU_FORMAT_RGB24 : DJVU_FORMAT_GRAY8;
                        refimg.stride = ref.rowsize;
                        refimg.data = ref.data;
                        hu_write_pnm(refpath, &refimg, 1); /* ref is BGR -> swap to RGB */
                    }
                    hu_write_pnm(minepath, mine, 1); /* ours is BGR -> swap to RGB */
                    printf("render\t%d\tmismatch\t%s\t%s\n",
                           i + 1, refpath, minepath);
                } else {
                    printf("render\t%d\tmismatch\n", i + 1);
                }
            }
            bench_render_free(&ref);
            djvu_image_destroy(ctx, mine);
            djvu_doc_drop_page_cache(doc, i);
            if (verify_mem_checkpoint(doc, i + 1, "after_page_free") < 0)
                goto mem_limit;
            verify_page_finish();
        }
        }
    }
    /* summary <0> <0> <0> <ok> <mismatch> <skip> <cosmetic>: cosmetic is the
       subset of <ok> pages accepted under the image_compare tolerance. */
    bench_ddjvu_purge();
    printf("summary\t0\t0\t0\t%d\t%d\t%d\t%d\n", m, mm, skip, cosmetic);
    return mm ? 1 : 0;
mem_limit:
    bench_ddjvu_purge();
    printf("summary\t0\t0\t0\t%d\t%d\t%d\t%d\n", m, mm, skip, cosmetic);
    return 3;
}

/* Text-only verify: one doc open; ref text on stdin (djvutxt multi-page blob). */
static int run_verify_text(djvu_doc *doc, const char *ref_blob, size_t ref_len)
{
    djvu_ctx *ctx = doc->ctx;
    int npages = djvu_doc_page_count(doc);
    char **ref_pages = NULL;
    int tm = 0, tmm = 0, te = 0;
    int i;

    if (ref_blob)
        ref_pages = read_ref_text_pages(ref_blob, ref_len, npages);

    for (i = 0; i < npages; i++) {
        char *mt = NULL, *rt = NULL, *mtn, *rtn;
        if (ref_pages)
            rt = ref_pages[i];
        mt = djvu_page_text(doc, i);
        mtn = text_normalize(mt ? mt : "");
        rtn = text_normalize(rt ? rt : "");
        if (mt) djvu_text_destroy(ctx, mt);
        if (!rtn[0] && !mtn[0]) {
            te++;
            printf("text\t%d\tempty\n", i + 1);
        } else if (!strcmp(rtn, mtn)) {
            tm++;
            printf("text\t%d\tok\n", i + 1);
        } else {
            tmm++;
            printf("text\t%d\tmismatch\n", i + 1);
        }
        free(mtn);
        free(rtn);
    }

    printf("summary\t0\t0\t0\t%d\t%d\t%d\n", tm, tmm, te);
    if (ref_pages) {
        for (i = 0; i < npages; i++)
            free(ref_pages[i]);
        free(ref_pages);
    }
    return tmm ? 1 : 0;
}

static void usage(void)
{
    fputs(
        "djvu_test -- djvudec test harness (verifies against DjVuLibre)\n"
        "\n"
        "  djvu_test [options] file.djvu\n"
        "\n"
        "Inspect (default with no option: -info):\n"
        "  -info              page count + per-page size, dpi, version, rotation\n"
        "  -comps             -info plus the DJVM component list\n"
        "  -page N            page number, 1-based (default 1)\n"
        "  -text              hidden text of the page\n"
        "  -zones             text zone tree with bounding boxes\n"
        "  -outline           document bookmark outline\n"
        "  -links             page hyperlinks\n"
        "  -type              page content type (bitonal/photo/compound)\n"
        "  -out FILE          render the page to PGM/PPM (- = stdout)\n"
        "  -sub N             subsample for -out (default 1)\n"
        "\n"
        "Verify:\n"
        "  -verify-render     compare every page render with ddjvu (cmd/tests.ts)\n"
        "  -diffdir DIR       with -verify-render: write ref/mine PNMs of mismatches\n"
        "  -verify-text       compare page text with the packed reference on stdin\n"
        "  -verify-into       djvu_page_render_into == djvu_page_render at sub 1,2,3\n"
        "  -verify-abort      abort API: tokens, djvu_request_abort, in-flight abort\n"
        "                     sweep, leak / stale-state checks (honors -page)\n"
        "\n"
        "Benchmark (ours vs DjVuLibre, same clock):\n"
        "  -bench             per-page djvu_page_render vs ddjvu_page_render\n"
        "  -bench-sum         same, replicating the SumatraPDF engine render paths\n"
        "  -profile-sum       loop the -bench-sum render of -page forever (profilers)\n"
        "\n"
        "Codec layers:\n"
        "  -bzzdec            decode the input as a raw BZZ stream to -out or stdout\n"
        "  -bg                composited background layer of the page (to -out)\n"
        "  -iwbg              BG44 IW44 layer (to -out)\n"
        "  -iwfg              FG44 IW44 layer (to -out)\n"
        "  -iwbggray          BG44 Y plane (to -out)\n"
        "  -iwbgcb            BG44 Cb plane (to -out)\n"
        "  -iwbgcr            BG44 Cr plane (to -out)\n"
        "  -iwdumpbg          dump BG44 chunks as FORM:PM44 to -out (default iw.djvu)\n"
        "  -iwdumpfg          dump FG44 chunks as FORM:PM44 to -out (default iw.djvu)\n"
        "\n"
        "  -h, -help          this text\n",
        stderr);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int do_info = 0, do_text = 0, do_bzz = 0, do_iw = 0, page = 1, out_sub = 1;
    int do_zones = 0, do_outline = 0, do_links = 0, do_type = 0, do_bench = 0;
    int do_verify_text = 0, do_verify_render = 0, do_verify_into = 0;
    int do_verify_abort = 0, do_profile_sum = 0, page_explicit = 0;
    const char *diffdir = NULL;
    int i, rc = 0;
    uint8_t *data; size_t len;
    djvu_ctx *ctx; djvu_doc *doc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-info")) do_info = 1;
        else if (!strcmp(argv[i], "-text")) do_text = 1;
        else if (!strcmp(argv[i], "-bzzdec")) do_bzz = 1;
        else if (!strcmp(argv[i], "-comps")) do_info = 2;
        else if (!strcmp(argv[i], "-bg")) do_iw = 8;         /* full-page background */
        else if (!strcmp(argv[i], "-iwbggray")) do_iw = 5;   /* BG44 Y plane */
        else if (!strcmp(argv[i], "-iwbgcb")) do_iw = 6;     /* BG44 Cb plane */
        else if (!strcmp(argv[i], "-iwbgcr")) do_iw = 7;     /* BG44 Cr plane */
        else if (!strcmp(argv[i], "-iwbg")) do_iw = 1;       /* render BG44 (mine) */
        else if (!strcmp(argv[i], "-iwfg")) do_iw = 2;       /* render FG44 (mine) */
        else if (!strcmp(argv[i], "-iwdumpbg")) do_iw = 3;   /* dump BG44 as PM44 */
        else if (!strcmp(argv[i], "-iwdumpfg")) do_iw = 4;   /* dump FG44 as PM44 */
        else if (!strcmp(argv[i], "-zones")) do_zones = 1;   /* text zone tree */
        else if (!strcmp(argv[i], "-outline")) do_outline = 1;
        else if (!strcmp(argv[i], "-links")) do_links = 1;
        else if (!strcmp(argv[i], "-type")) do_type = 1;
        else if (!strcmp(argv[i], "-bench")) do_bench = 1;
        else if (!strcmp(argv[i], "-bench-sum")) do_bench = 2; /* SumatraPDF Engine* render path */
        else if (!strcmp(argv[i], "-profile-sum")) do_profile_sum = 1;
        else if (!strcmp(argv[i], "-verify-into")) do_verify_into = 1;
        else if (!strcmp(argv[i], "-verify-text")) do_verify_text = 1;
        else if (!strcmp(argv[i], "-verify-render")) do_verify_render = 1;
        else if (!strcmp(argv[i], "-verify-abort")) do_verify_abort = 1;
        else if (!strcmp(argv[i], "-diffdir") && i + 1 < argc) diffdir = argv[++i];
        else if (!strcmp(argv[i], "-page") && i + 1 < argc) {
            page = atoi(argv[++i]);
            page_explicit = 1;
        }
        else if (!strcmp(argv[i], "-sub") && i + 1 < argc) out_sub = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "-help") ||
                 !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        else if (argv[i][0] == '-') {
            /* An unknown flag (or a value flag missing its value) must not be
               mistaken for the input file name. */
            fprintf(stderr, "djvu_test: unknown option or missing value: %s\n\n",
                    argv[i]);
            usage();
            return 2;
        }
        else in = argv[i];
    }
    if (!in) {
        usage();
        return 2;
    }

    data = hu_read_file(in, &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    djvu_init();

    /* -verify-abort builds its own contexts (counting allocator, cache on/off) */
    if (do_verify_abort) {
        rc = run_verify_abort(data, len, page_explicit ? page : 0);
        free(data);
        return rc;
    }

    /* Verify: lazy decode per chunk (tests.ts sets DJVU_VERIFY_LO/HI). */
    if (do_verify_render)
        verify_install_crash_handlers();

    /* -verify-render installs the accounting allocator (per-file memory report,
       leak check, 4 GB-per-ctx abort); other modes use the default allocator so
       e.g. -bench timing isn't perturbed. */
    if (do_verify_render)
        ctx = djvu_ctx_new(mem_alloc, mem_free, hu_lock_cb, hu_unlock_cb,
                           hu_on_error, NULL);
    else
        ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, hu_on_error, NULL);
    if (!ctx) {
        fprintf(stderr, "djvu_ctx_new failed\n");
        free(data);
        return 1;
    }
    if (do_verify_render)
        djvu_ctx_set_cache_per_page(ctx, 1);

    if (do_profile_sum) {
        djvu_ctx_set_bgr(ctx, 1);
        rc = profile_sum_page(ctx, data, len, page - 1);
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

    if (do_bzz) {
        /* decode a raw BZZ stream and write to -out (or stdout) */
        size_t olen = 0;
        uint8_t *o = djvu_bzz_decode_all(ctx, data, len, &olen);
        if (!o) {
            fprintf(stderr, "bzzdec: decode failed\n");
            rc = 1;
        } else {
            FILE *f = out ? fopen(out, "wb") : stdout;
            if (!out) hu_stdout_binary(); /* raw bytes: no CRLF translation */
            if (!f) {
                fprintf(stderr, "cannot write %s\n", out);
                rc = 1;
            } else {
                if (fwrite(o, 1, olen, f) != olen) rc = 1;
                if (out ? fclose(f) != 0 : fflush(f) != 0) rc = 1;
            }
            djvu_free(ctx, o);
        }
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }
    doc = djvu_doc_open(ctx, data, len);
    if (!doc) { fprintf(stderr, "cannot open document\n"); free(data); djvu_ctx_free(ctx); return 1; }

    if (do_verify_render) {
        rc = run_verify_render(doc, in, diffdir);
        djvu_doc_close(doc);
        djvu_ctx_free(ctx);
        free(data);
        /* report totals/peak + verify everything was freed (after ctx free) */
        return mem_finish(rc);
    }

    if (do_verify_into) {
        rc = run_verify_into(doc);
        djvu_doc_close(doc);
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

    if (do_verify_text) {
        char *ref_blob = NULL;
        size_t ref_len = 0;
#if defined(_WIN32)
        if (!_isatty(_fileno(stdin))) {
            _setmode(_fileno(stdin), _O_BINARY);
            ref_blob = read_all_stdin(&ref_len);
        }
#else
        if (!isatty(fileno(stdin)))
            ref_blob = read_all_stdin(&ref_len);
#endif
        rc = run_verify_text(doc, ref_blob, ref_len);
        free(ref_blob);
        djvu_doc_close(doc);
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

    if (do_iw) {
        if (do_iw == 3 || do_iw == 4) {
            rc = djvu_debug_dump_iw(doc, page - 1, do_iw == 4, out ? out : "iw.djvu");
        } else if (do_iw == 8) {
            djvu_image *img = djvu_debug_render_bg(doc, page - 1);
            if (img) { if (out) hu_write_pnm(out, img, 0); djvu_image_destroy(ctx, img); }
            else rc = 1;
        } else if (do_iw == 5 || do_iw == 6 || do_iw == 7) {
            djvu_image *img = djvu_debug_render_iw_plane(doc, page - 1, 0, do_iw - 5);
            if (img) { if (out) hu_write_pnm(out, img, 0); djvu_image_destroy(ctx, img); }
            else rc = 1;
        } else {
            djvu_image *img = djvu_debug_render_iw(doc, page - 1, do_iw == 2);
            if (img) {
                if (out) hu_write_pnm(out, img, 0);
                djvu_image_destroy(ctx, img);
            } else rc = 1;
        }
        djvu_doc_close(doc); djvu_ctx_free(ctx); free(data);
        return rc;
    }

    if (do_bench) {
        int n = djvu_doc_page_count(doc);
        int sum = (do_bench == 2); /* replicate SumatraPDF Engine* render path */
        const int RUNS = 2;
        bench_session_timings ours[2], lib[2];
        double *page_ms[4];
        int r, p;

        djvu_doc_close(doc);
        doc = NULL;
        djvu_ctx_free(ctx);
        ctx = bench_ctx_new();
        if (!ctx) {
            fprintf(stderr, "bench: out of memory\n");
            free(data);
            return 1;
        }
        bench_ctx_configure(ctx, 0, sum);
        bench_ddjvu_reset();

        for (i = 0; i < 4; i++) {
            page_ms[i] = (double *)malloc((size_t)n * sizeof(double));
            if (!page_ms[i]) {
                while (--i >= 0) free(page_ms[i]);
                fprintf(stderr, "bench: out of memory\n");
                djvu_ctx_free(ctx);
                free(data);
                return 1;
            }
        }
        for (r = 0; r < RUNS; r++) {
            memset(&ours[r], 0, sizeof ours[r]);
            ours[r].page_ms = page_ms[r];
            ours[r].npages = n;
        }
        for (r = 0; r < RUNS; r++) {
            memset(&lib[r], 0, sizeof lib[r]);
            lib[r].page_ms = page_ms[r + 2];
            lib[r].npages = n;
        }

        /* Interleave the sessions (ours, lib, ours, lib): running all of one
           side back-to-back lets a transient machine-load difference between
           the two phases masquerade as per-page speed differences. */
        for (r = 0; r < RUNS; r++) {
            if (bench_ours_session(ctx, data, len, sum, &ours[r]) != 0)
                fprintf(stderr, "bench: djvudec session run %d failed\n", r + 1);
            if (bench_ddjvu_session(in, sum, &lib[r]) != 0)
                fprintf(stderr, "bench: libdjvu session run %d failed\n", r + 1);
        }

        {
            double open_ours = bench_best2(ours[0].open_ms, ours[1].open_ms);
            double open_lib = bench_best2(lib[0].open_ms, lib[1].open_ms);
            double close_ours = bench_best2(ours[0].close_ms, ours[1].close_ms);
            double close_lib = bench_best2(lib[0].close_ms, lib[1].close_ms);
            double total_ours = bench_best2(ours[0].total_ms, ours[1].total_ms);
            double total_lib = bench_best2(lib[0].total_ms, lib[1].total_ms);

            bench_cmp_row *rows = (bench_cmp_row *)calloc((size_t)n + 3, sizeof *rows);

            if (!rows) {
                fprintf(stderr, "bench: out of memory\n");
                for (i = 0; i < 4; i++)
                    free(page_ms[i]);
                free(data);
                return 1;
            }
            strcpy(rows[0].op, "open");
            rows[0].ours = open_ours;
            rows[0].lib = open_lib;
            for (p = 0; p < n; p++) {
                snprintf(rows[1 + p].op, sizeof rows[1 + p].op, "%d", p + 1);
                rows[1 + p].ours = bench_best2(ours[0].page_ms[p], ours[1].page_ms[p]);
                rows[1 + p].lib = bench_best2(lib[0].page_ms[p], lib[1].page_ms[p]);
            }
            strcpy(rows[n + 1].op, "close");
            rows[n + 1].ours = close_ours;
            rows[n + 1].lib = close_lib;
            strcpy(rows[n + 2].op, "total");
            rows[n + 2].ours = total_ours;
            rows[n + 2].lib = total_lib;

            bench_print_compare_table(rows, n + 3);
            free(rows);
        }

        for (i = 0; i < 4; i++)
            free(page_ms[i]);
        bench_mem_report(data, len, sum);
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

    if (do_info == 2) djvu_debug_dump_comps(doc);

    if (do_info || (!out && !do_text && !do_zones && !do_outline && !do_links && !do_type)) {
        int n = djvu_doc_page_count(doc);
        printf("pages: %d\n", n);
        for (i = 0; i < n; i++) {
            djvu_page_info pi;
            if (djvu_doc_page_info(doc, i, &pi) == 0)
                printf("page %d: %dx%d dpi=%d ver=%d rot=%d\n",
                       i + 1, pi.width, pi.height, pi.dpi, pi.version, pi.rotation);
            else
                printf("page %d: <no info>\n", i + 1);
        }
    }

    if (do_text) {
        char *t = djvu_page_text(doc, page - 1);
        if (t) { fputs(t, stdout); djvu_text_destroy(ctx, t); }
    }

    if (do_type) {
        printf("page %d type: %s\n", page,
               hu_page_type_name(djvu_page_get_type(doc, page - 1)));
    }

    if (do_zones) {
        djvu_page_text_zones *z = djvu_page_text_get_zones(doc, page - 1);
        if (z && z->root) hu_print_zone(z->root, 0);
        else printf("(no text zones)\n");
        djvu_text_zones_destroy(ctx, z);
    }

    if (do_outline) {
        djvu_outline_item *root = djvu_doc_outline(doc);
        if (root) hu_print_outline(root, -1);
        else printf("(no outline)\n");
        djvu_outline_destroy(ctx, root);
    }

    if (do_links) {
        djvu_page_links *L = djvu_page_get_links(doc, page - 1);
        hu_print_links(L);
        if (L) djvu_page_links_destroy(ctx, L);
    }

    if (out) {
        djvu_image *img = djvu_page_render(doc, page - 1, out_sub);
        if (img) {
            if (hu_write_pnm(out, img, 0) != 0) { fprintf(stderr, "cannot write %s\n", out); rc = 1; }
            djvu_image_destroy(ctx, img);
        } else { rc = 1; }
    }

    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    free(data);
    return rc;
}
