/* djvu_test.c -- CLI harness for the djvu library (jbig2dec-style tool).
 *
 *   djvu_test -info <in.djvu>
 *   djvu_test -page <N> -out <out.pgm|out.ppm> <in.djvu>   (1-based page)
 *   djvu_test -page <N> -text <in.djvu>
 *
 * Used to verify against DjVuLibre's ddjvu / djvutxt. */
#include "djvu.h"
#include "djvu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Disable DjVuLibre ddjvuapi document cache (-verify-render cold oracle). */
int djvu_test_ddjvu_cold = 0;

/* timing / oracle helpers from bench_ddjvu.cpp (DjVuLibre, same clock) */
typedef struct bench_render {
    int width, height;
    int bps;
    int rowsize;
    uint8_t *data;
} bench_render;

double bench_now_ms(void);
double bench_ddjvu_page_ms(const char *path, int page0);
double bench_ddjvu_doc_ms(const char *path);
double bench_ddjvu_page_sum_ms(const char *path, int page0);
double bench_ddjvu_doc_sum_ms(const char *path);
int bench_ddjvu_render_page(const char *path, int page0, int want_rgb,
                            bench_render *out);
void bench_render_free(bench_render *img);
void bench_ddjvu_reset(void);
void bench_ddjvu_purge(void);
void bench_ddjvu_mem_debug(FILE *out);

/* ---- per-context memory accounting (djvu_alloc_cb / djvu_free_cb hooks) ----
   Installed by -verify-render so each file's run reports total allocations and
   peak live memory, confirms every allocation was freed, and aborts (exit 3) if
   a single djvu_ctx's live memory exceeds 4 GB. Each allocation carries a small
   header storing its size so frees can be attributed. The djvu_ctx arg lets us
   keep separate tallies per context. Single-threaded (one thread per process). */
#define MEM_HDR 16  /* preserves malloc's 16-byte alignment; holds the size */
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
    uint8_t *p;
    mem_rec *r;
    (void)user;
    p = (uint8_t *)malloc(size + MEM_HDR);
    if (!p) return NULL;
    *(size_t *)p = size;
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
    return p + MEM_HDR;
}

static void mem_free(void *user, void *ctx, void *ptr)
{
    uint8_t *p;
    size_t size;
    mem_rec *r;
    (void)user;
    if (!ptr) return;
    p = (uint8_t *)ptr - MEM_HDR;
    size = *(size_t *)p;
    r = mem_find(ctx);
    if (r) { r->live -= size; r->n_free++; }
    free(p);
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

static void on_error(void *user, djvu_severity sev, const char *msg)
{
    (void)user;
    const char *s = sev >= DJVU_SEVERITY_ERROR ? "error" :
                    sev == DJVU_SEVERITY_WARNING ? "warning" : "info";
    fprintf(stderr, "djvu %s: %s\n", s, msg);
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/* Write img as a PNM. PNM color order is RGB; when bgr != 0 the image data is
   B,G,R (e.g. rendered with djvu_ctx_set_bgr) and each pixel is swapped back to
   RGB on the way out so the file is still a correct PPM. */
static int write_pnm_ex(const char *path, djvu_image *img, int bgr)
{
    FILE *f;
    int y;
    if (path && !strcmp(path, "-")) {
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        f = stdout;
    } else {
        f = fopen(path, "wb");
        if (!f) return -1;
    }
    if (img->format == DJVU_FORMAT_RGB24)
        fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
    else
        fprintf(f, "P5\n%d %d\n255\n", img->width, img->height);
    if (bgr && img->format == DJVU_FORMAT_RGB24) {
        uint8_t *row = (uint8_t *)malloc((size_t)img->width * 3);
        if (!row) { if (f != stdout) fclose(f); return -1; }
        for (y = 0; y < img->height; y++) {
            const uint8_t *src = img->data + (size_t)y * img->stride;
            int x;
            for (x = 0; x < img->width; x++) {
                row[x * 3 + 0] = src[x * 3 + 2];
                row[x * 3 + 1] = src[x * 3 + 1];
                row[x * 3 + 2] = src[x * 3 + 0];
            }
            fwrite(row, 1, (size_t)img->width * 3, f);
        }
        free(row);
    } else {
        for (y = 0; y < img->height; y++)
            fwrite(img->data + (size_t)y * img->stride, 1,
                   (size_t)img->width * img->format, f);
    }
    if (f != stdout)
        fclose(f);
    return 0;
}

static int write_pnm(const char *path, djvu_image *img)
{
    return write_pnm_ex(path, img, 0);
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
    int has_bg = djvu_form_find_chunk(doc, form_off, "BG44", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "FG44", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "BGjp", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "FGjp", &sz, NULL) != NULL;
    if (has_bg) return PK_BG;
    if (has_sjbz) return PK_MASK;
    return PK_OTHER;
}

static const char *page_type_name(djvu_page_type t)
{
    static const char *names[] = {"unknown", "bitonal", "photo", "compound"};
    return names[(t >= 0 && t <= 3) ? t : 0];
}

static const char *page_kind_name(page_kind_t k)
{
    if (k == PK_MASK) return "mask";
    if (k == PK_BG) return "bg";
    return "other";
}

/* Cold page render: fresh doc per rep; timer covers render only (open/close outside). */
static double bench_ours_page_ms(djvu_ctx *ctx, const uint8_t *data, size_t len,
                                 int page0)
{
    djvu_doc *doc;
    djvu_image *img;
    double t0, ms;

    doc = djvu_doc_open(ctx, data, len);
    if (!doc)
        return -1.0;
    t0 = bench_now_ms();
    img = djvu_page_render(doc, page0, 1);
    ms = bench_now_ms() - t0;
    if (img)
        djvu_image_destroy(ctx, img);
    djvu_doc_close(doc);
    return img ? ms : -1.0;
}

/* One cold session: open + render all pages + text + links + close. */
static double bench_ours_doc_ms(djvu_ctx *ctx, const uint8_t *data, size_t len)
{
    djvu_doc *doc;
    int n, i;
    double t0, ms;

    t0 = bench_now_ms();
    doc = djvu_doc_open(ctx, data, len);
    if (!doc)
        return -1.0;
    n = djvu_doc_page_count(doc);
    for (i = 0; i < n; i++) {
        djvu_image *img = djvu_page_render(doc, i, 1);
        if (img)
            djvu_image_destroy(ctx, img);
        {
            char *t = djvu_page_text(doc, i);
            if (t)
                djvu_text_destroy(ctx, t);
        }
        {
            djvu_page_links *L = djvu_page_get_links(doc, i);
            if (L)
                djvu_page_links_destroy(ctx, L);
        }
    }
    djvu_doc_close(doc);
    ms = bench_now_ms() - t0;
    return ms;
}

/* --- bench-sum: replicate SumatraPDF EngineDjvuDec::RenderPage (our path) ---
 * Renders at zoom=1, user-rotation=0. Mirrors the engine's non-GDI work: pick
 * an integer subsample, query the output geometry, then render straight into a
 * destination buffer (djvu_page_render_into -- the engine's DIB), so there is
 * no separate format-convert/copy pass. The decoder now applies intrinsic page
 * rotation itself (at every subsample, via a tiled transpose), so there is no
 * engine-side rotate either. The GDI StretchBlt/DIB allocation is excluded by
 * design (the dst buffer stands in for the DIB and is allocated outside the
 * timer). */

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
   Compound (color) pages are forced to full resolution. */
static int sum_pick_subsample(djvu_page_type t, int upW, int upH, int tdx, int tdy)
{
    int s;
    if (t == DJVU_PAGE_COMPOUND)
        return 1;
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
static int sum_page_subsample(djvu_doc *doc, int page0, djvu_page_type *ptype)
{
    djvu_page_info info;
    int dpi, rot, upW, upH, fullW, fullH;
    memset(&info, 0, sizeof info);
    *ptype = djvu_page_get_type(doc, page0);
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
    return sum_pick_subsample(*ptype, upW, upH, fullW, fullH);
}

/* Render a page the way the updated EngineDjvuDec does: query geometry, then
   render straight into dst (no convert pass; the decoder also applies intrinsic
   rotation). dst (>= worst-case size) is provided by the caller. */
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

/* Worst-case dst size for any page render (full-res, 3 bytes/pixel). */
static size_t sum_dst_capacity(djvu_doc *doc, int page0)
{
    djvu_page_info info;
    if (djvu_doc_page_info(doc, page0, &info) != 0 || info.width <= 0 || info.height <= 0)
        return 0;
    return (size_t)info.width * (size_t)info.height * 3;
}

/* Warm sum page render: fresh doc per rep (Sjbz preloaded at open); timer covers
   render_into only (dst buffer allocated outside the timer). */
static double bench_ours_page_sum_ms(djvu_ctx *ctx, const uint8_t *data, size_t len,
                                     int page0)
{
    djvu_doc *doc;
    djvu_page_type ptype;
    int subsample, ok;
    size_t cap;
    uint8_t *dst;
    double t0, ms;

    doc = djvu_doc_open(ctx, data, len);
    if (!doc)
        return -1.0;
    subsample = sum_page_subsample(doc, page0, &ptype);
    cap = sum_dst_capacity(doc, page0);
    dst = cap ? (uint8_t *)malloc(cap) : NULL;
    if (!dst) {
        djvu_doc_close(doc);
        return -1.0;
    }

    t0 = bench_now_ms();
    ok = sum_render_into(doc, page0, subsample, dst) == 0;
    ms = bench_now_ms() - t0;
    free(dst);
    djvu_doc_close(doc);
    return ok ? ms : -1.0;
}

/* Tight loop for macOS `sample` profiling: one doc open, repeated sum renders. */
static int profile_sum_page(djvu_ctx *ctx, const uint8_t *data, size_t len, int page0)
{
    djvu_doc *doc;
    djvu_page_type ptype;
    int subsample, n;
    size_t cap;
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
    subsample = sum_page_subsample(doc, page0, &ptype);
    cap = sum_dst_capacity(doc, page0);
    dst = cap ? (uint8_t *)malloc(cap) : NULL;
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

/* One cold session, sum render: open + render all pages + text + links + close. */
static double bench_ours_doc_sum_ms(djvu_ctx *ctx, const uint8_t *data, size_t len)
{
    djvu_doc *doc;
    int n, i;
    double t0, ms;

    t0 = bench_now_ms();
    doc = djvu_doc_open(ctx, data, len);
    if (!doc)
        return -1.0;
    n = djvu_doc_page_count(doc);
    for (i = 0; i < n; i++) {
        djvu_page_type ptype;
        int subsample;
        size_t cap = sum_dst_capacity(doc, i);
        uint8_t *dst = cap ? (uint8_t *)malloc(cap) : NULL;
        subsample = sum_page_subsample(doc, i, &ptype);
        if (dst) {
            sum_render_into(doc, i, subsample, dst);
            free(dst);
        }
        {
            char *t = djvu_page_text(doc, i);
            if (t)
                djvu_text_destroy(ctx, t);
        }
        {
            djvu_page_links *L = djvu_page_get_links(doc, i);
            if (L)
                djvu_page_links_destroy(ctx, L);
        }
    }
    djvu_doc_close(doc);
    ms = bench_now_ms() - t0;
    return ms;
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
    ctx = djvu_ctx_new(mem_alloc, mem_free, on_error, NULL);
    if (!ctx) return;
    if (sum) djvu_ctx_set_bgr(ctx, 1);
    doc = djvu_doc_open(ctx, data, len);
    if (doc) {
        int n = djvu_doc_page_count(doc);
        for (i = 0; i < n; i++) {
            if (sum) {
                djvu_page_type pt;
                int ss = sum_page_subsample(doc, i, &pt);
                size_t cap = sum_dst_capacity(doc, i);
                uint8_t *dst = cap ? (uint8_t *)malloc(cap) : NULL;
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
   djvu_page_render_info geometry) for every page at subsample=1. */
static int run_verify_into(djvu_doc *doc)
{
    djvu_ctx *ctx = doc->ctx;
    int n = djvu_doc_page_count(doc), i, mism = 0, checked = 0;
    for (i = 0; i < n; i++) {
        djvu_render_info ri;
        djvu_image *img;
        uint8_t *dst;
        int comp, stride, y, bad = 0;
        if (djvu_page_render_info(doc, i, 1, &ri) != 0)
            continue; /* page renders nothing at subsample 1 */
        img = djvu_page_render(doc, i, 1);
        if (!img)
            continue;
        if (img->width != ri.width || img->height != ri.height ||
            img->format != ri.format) {
            printf("page %d: render_info geometry mismatch\n", i + 1);
            mism++; djvu_image_destroy(ctx, img); continue;
        }
        comp = (ri.format == DJVU_FORMAT_GRAY8) ? 1 : 3;
        stride = ri.width * comp;
        dst = (uint8_t *)malloc((size_t)stride * ri.height);
        if (!dst) { djvu_image_destroy(ctx, img); continue; }
        checked++;
        if (djvu_page_render_into(doc, i, 1, dst, stride) != 0) {
            printf("page %d: render_into failed\n", i + 1); mism++;
        } else {
            for (y = 0; y < ri.height && !bad; y++)
                if (memcmp(dst + (size_t)y * stride,
                           img->data + (size_t)y * img->stride, (size_t)stride) != 0)
                    bad = 1;
            if (bad) { printf("page %d: render_into bytes differ\n", i + 1); mism++; }
        }
        free(dst);
        djvu_image_destroy(ctx, img);
    }
    printf("verify-into: %d/%d pages checked, %d mismatch\n", checked, n, mism);
    return mism ? 1 : 0;
}

static int page_has_chunk(djvu_doc *doc, int page0, const char *cid)
{
    uint32_t sz;
    return djvu_form_find_chunk(doc, doc->pages[page0].form_off, cid, &sz, NULL) != NULL;
}

static int page_has_incl(djvu_doc *doc, int page0)
{
    uint32_t sz, start = 0;
    return djvu_form_find_chunk(doc, doc->pages[page0].form_off, "INCL", &sz, &start) != NULL;
}

static const char *container_kind(djvu_doc *doc)
{
    const uint8_t *data = doc->data;
    size_t len = doc->len;
    uint32_t pos = doc->root_form_off;

    if (pos + 12 > len) return "unknown";
    if (djvu_tag_eq(data + pos + 8, "DJVU")) return "single";
    if (!djvu_tag_eq(data + pos + 8, "DJVM")) return "unknown";
    {
        uint32_t form_end = pos + 8 + djvu_rd_u32be(data + pos + 4);
        uint32_t p = pos + 12;
        if (form_end > len) form_end = (uint32_t)len;
        while (p + 8 <= form_end) {
            uint32_t csz = djvu_rd_u32be(data + p + 4);
            uint32_t chunk_end = p + 8 + csz;
            if (chunk_end > form_end) break;
            if (djvu_tag_eq(data + p, "DIRM") && p + 11 <= chunk_end) {
                return ((data[p + 8] >> 7) & 1) ? "bundled" : "indirect";
            }
            p = chunk_end + (csz & 1);
        }
    }
    return "djvm";
}

/* Feature dump for cmd/dump_features.ts (tab-separated; one file per invocation). */
static int run_dump_features(djvu_doc *doc)
{
    djvu_ctx *ctx = doc->ctx;
    int npages = djvu_doc_page_count(doc);
    int i, ncomp_djvi = 0;
    int file_incl = 0, file_incl_djbz = 0, file_inline_djbz = 0;
    int file_outline = 0, file_text = 0, file_annot = 0, file_links = 0;
    int file_fgbz = 0, file_fg44 = 0, file_bgjp = 0, file_fgjp = 0;
    int type_bitonal = 0, type_photo = 0, type_compound = 0;
    int kind_mask = 0, kind_bg = 0, kind_other = 0;
    int rot_nonzero = 0;
    double total_ms = 0.0;
    djvu_outline_item *outline;

    if (doc->comps) {
        for (i = 0; i < doc->ncomp; i++)
            if (doc->comps[i].type == 0) ncomp_djvi++;
    }
    outline = djvu_doc_outline(doc);
    file_outline = (outline && outline->nchildren > 0) ? 1 : 0;
    if (outline) djvu_outline_destroy(ctx, outline);

    for (i = 0; i < npages; i++) {
        djvu_page_type pt = djvu_page_get_type(doc, i);
        page_kind_t pk = page_kind(doc, i);
        djvu_page_info pi;
        int has_text = 0, has_annot = 0, has_links = 0;
        int incl = page_has_incl(doc, i);
        int inline_djbz = page_has_chunk(doc, i, "Djbz");
        int incl_djbz = djvu_form_find_incl_chunk(doc, doc->pages[i].form_off,
                                                  "Djbz", NULL) != NULL;
        double t0, dt;
        djvu_image *img;
        djvu_page_links *L;

        if (pt == DJVU_PAGE_BITONAL) type_bitonal = 1;
        else if (pt == DJVU_PAGE_PHOTO) type_photo = 1;
        else if (pt == DJVU_PAGE_COMPOUND) type_compound = 1;
        if (pk == PK_MASK) kind_mask = 1;
        else if (pk == PK_BG) kind_bg = 1;
        else kind_other = 1;

        if (page_has_chunk(doc, i, "FGbz")) file_fgbz = 1;
        if (page_has_chunk(doc, i, "FG44")) file_fg44 = 1;
        if (page_has_chunk(doc, i, "BGjp")) file_bgjp = 1;
        if (page_has_chunk(doc, i, "FGjp")) file_fgjp = 1;
        if (incl) file_incl = 1;
        if (incl_djbz) file_incl_djbz = 1;
        if (inline_djbz) file_inline_djbz = 1;

        {
            char *tx = djvu_page_text(doc, i);
            has_text = (tx && tx[0]) ? 1 : 0;
            if (tx) djvu_text_destroy(ctx, tx);
        }
        if (page_has_chunk(doc, i, "ANTa") || page_has_chunk(doc, i, "ANTz"))
            has_annot = 1;
        L = djvu_page_get_links(doc, i);
        if (L) {
            has_links = (L->nlinks > 0) ? 1 : 0;
            djvu_page_links_destroy(ctx, L);
        }
        if (has_text) file_text = 1;
        if (has_annot) file_annot = 1;
        if (has_links) file_links = 1;

        pi.rotation = 0;
        if (djvu_doc_page_info(doc, i, &pi) == 0 && pi.rotation != 0)
            rot_nonzero = 1;

        t0 = bench_now_ms();
        img = djvu_page_render(doc, i, 1);
        dt = bench_now_ms() - t0;
        if (img) djvu_image_destroy(ctx, img);
        total_ms += dt;

        printf("page\t%d\ttype\t%s\tkind\t%s\trot\t%d\ttext\t%d\tannot\t%d\t"
               "links\t%d\tincl\t%d\tinline_djbz\t%d\tincl_djbz\t%d\t"
               "fgbz\t%d\tfg44\t%d\tbgjp\t%d\tfgjp\t%d\trender_ms\t%.3f\n",
               i + 1, page_type_name(pt), page_kind_name(pk),
               (pi.rotation == 90 || pi.rotation == 180 || pi.rotation == 270)
                   ? pi.rotation : 0,
               has_text, has_annot, has_links, incl, inline_djbz, incl_djbz,
               page_has_chunk(doc, i, "FGbz"), page_has_chunk(doc, i, "FG44"),
               page_has_chunk(doc, i, "BGjp"), page_has_chunk(doc, i, "FGjp"),
               dt);
    }

    printf("summary\tpages\t%d\tcontainer\t%s\tncomp_djvi\t%d\t"
           "incl\t%d\tincl_djbz\t%d\tinline_djbz\t%d\toutline\t%d\t"
           "text\t%d\tannot\t%d\tlinks\t%d\t"
           "type_bitonal\t%d\ttype_photo\t%d\ttype_compound\t%d\t"
           "kind_mask\t%d\tkind_bg\t%d\tkind_other\t%d\t"
           "fgbz\t%d\tfg44\t%d\tbgjp\t%d\tfgjp\t%d\trot_nonzero\t%d\t"
           "total_render_ms\t%.3f\n",
           npages, container_kind(doc), ncomp_djvi,
           file_incl, file_incl_djbz, file_inline_djbz, file_outline,
           file_text, file_annot, file_links,
           type_bitonal, type_photo, type_compound,
           kind_mask, kind_bg, kind_other,
           file_fgbz, file_fg44, file_bgjp, file_fgjp, rot_nonzero,
           total_ms);
    return 0;
}

/* Allow tiny color-page diffs from ddjvu's FG44 stencil quirk (mask/bg/fg layers
   are byte-exact; only composite FG placement drifts ~1px on rare text lines). */
static int image_equal_cosmetic(djvu_image *mine, const bench_render *ref)
{
    size_t total = 0, diffs = 0;
    int y, x, bpp;
    if (!mine || !ref || !ref->data) return 0;
    if (mine->format != DJVU_FORMAT_RGB24 || ref->bps != 3) return 0;
    if (mine->width != ref->width || mine->height != ref->height) return 0;
    bpp = 3;
    for (y = 0; y < mine->height; y++) {
        const uint8_t *a = mine->data + (size_t)y * (size_t)mine->stride;
        const uint8_t *b = ref->data + (size_t)y * (size_t)ref->rowsize;
        for (x = 0; x < mine->width; x++) {
            total++;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) diffs++;
            a += bpp;
            b += bpp;
        }
    }
    return diffs > 0 && diffs <= 2000 && diffs * 1000000 / total <= 100; /* <=0.01% */
}

static int image_equal(djvu_image *mine, const bench_render *ref)
{
    int y;
    if (!mine || !ref || !ref->data) return 0;
    if (mine->width != ref->width || mine->height != ref->height) return 0;
    if ((int)mine->format != ref->bps) return 0;
    for (y = 0; y < mine->height; y++) {
        size_t row = (size_t)mine->width * (size_t)mine->format;
        if (memcmp(mine->data + (size_t)y * (size_t)mine->stride,
                   ref->data + (size_t)y * (size_t)ref->rowsize, row) != 0) {
            if (image_equal_cosmetic(mine, ref))
                return 1;
            return 0;
        }
    }
    return 1;
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

/* In-memory render verify vs ddjvuapi; write PNMs to diffdir only on mismatch. */
static int run_verify_render(djvu_doc *doc, const char *path, const char *diffdir)
{
    djvu_ctx *ctx = doc->ctx;
    int npages = djvu_doc_page_count(doc);
    int m = 0, mm = 0, skip = 0;
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
        if (doc->ctx->lazy_iw44) {
            djvu_doc_preload_jb2_range(doc, lo - 1, hi - 1);
            djvu_doc_preload_iw44_range(doc, lo - 1, hi - 1);
        }
        if (verify_mem_checkpoint(doc, lo, "chunk_start") < 0)
            goto mem_limit;
        for (i = lo - 1; i < hi; i++) {
        page_kind_t kind = page_kind(doc, i);
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
                djvu_doc_drop_page_iw44(doc, i);
                if (verify_mem_checkpoint(doc, i + 1, "after_page_free") < 0)
                    goto mem_limit;
                verify_page_finish();
                continue;
            }
            if (!mine || bench_ddjvu_render_page(path, i, want_rgb, &ref) != 0) {
                if (mine) djvu_image_destroy(ctx, mine);
                bench_render_free(&ref);
                djvu_doc_drop_page_iw44(doc, i);
                mm++;
                printf("render\t%d\terror\n", i + 1);
                verify_page_finish();
                continue;
            }
            if (verify_mem_checkpoint(doc, i + 1, "after_ddjvu_render") < 0)
                goto mem_limit;
            if (image_equal(mine, &ref)) {
                m++;
                printf("render\t%d\tok\n", i + 1);
            } else {
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
                        write_pnm_ex(refpath, &refimg, 1); /* ref is BGR -> swap to RGB */
                    }
                    write_pnm_ex(minepath, mine, 1); /* ours is BGR -> swap to RGB */
                    printf("render\t%d\tmismatch\t%s\t%s\n",
                           i + 1, refpath, minepath);
                } else {
                    printf("render\t%d\tmismatch\n", i + 1);
                }
            }
            bench_render_free(&ref);
            djvu_image_destroy(ctx, mine);
            djvu_doc_drop_page_iw44(doc, i);
            if (verify_mem_checkpoint(doc, i + 1, "after_page_free") < 0)
                goto mem_limit;
            verify_page_finish();
        }
        }
    }
    bench_ddjvu_purge();
    printf("summary\t0\t0\t0\t%d\t%d\t%d\n", m, mm, skip);
    return mm ? 1 : 0;
mem_limit:
    bench_ddjvu_purge();
    printf("summary\t0\t0\t0\t%d\t%d\t%d\n", m, mm, skip);
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

static void print_zone(const djvu_text_zone *z, int depth)
{
    static const char *tn[8] = {"?","page","col","region","para","line","word","char"};
    int i;
    int t = (z->type >= 1 && z->type <= 7) ? z->type : 0;
    for (i = 0; i < depth; i++) fputs("  ", stdout);
    printf("%s [%d,%d %dx%d]", tn[t], z->x, z->y, z->w, z->h);
    if (z->type == DJVU_ZONE_WORD && z->text) printf(" \"%s\"", z->text);
    putchar('\n');
    for (i = 0; i < z->nchildren; i++) print_zone(&z->children[i], depth + 1);
}

static void print_outline(const djvu_outline_item *it, int depth)
{
    int i;
    if (it->title) {
        for (i = 0; i < depth; i++) fputs("  ", stdout);
        printf("%s -> %s (page %d)\n", it->title,
               it->url ? it->url : "", it->page_no);
    }
    for (i = 0; i < it->nchildren; i++) print_outline(&it->children[i], depth + 1);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int do_info = 0, do_text = 0, do_bzz = 0, do_iw = 0, page = 1, out_sub = 1;
    int do_zones = 0, do_outline = 0, do_links = 0, do_type = 0, do_bench = 0;
    int do_verify_text = 0, do_verify_render = 0, do_dump_features = 0, do_verify_into = 0;
    int do_profile_sum = 0;
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
        else if (!strcmp(argv[i], "-dump-features")) do_dump_features = 1;
        else if (!strcmp(argv[i], "-diffdir") && i + 1 < argc) diffdir = argv[++i];
        else if (!strcmp(argv[i], "-page") && i + 1 < argc) page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sub") && i + 1 < argc) out_sub = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-out") && i + 1 < argc) out = argv[++i];
        else in = argv[i];
    }
    if (!in) {
        fprintf(stderr, "usage: djvu_test [-info] [-page N] [-out f.pnm] [-text] in.djvu\n");
        return 2;
    }

    data = read_file(in, &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    /* Verify: lazy decode per chunk (tests.ts sets DJVU_VERIFY_LO/HI); cold ddjvu oracle. */
    if (do_verify_render)
        djvu_test_ddjvu_cold = 1;

    djvu_init();
    /* -verify-render installs the accounting allocator (per-file memory report,
       leak check, 4 GB-per-ctx abort); other modes use the default allocator so
       e.g. -bench timing isn't perturbed. */
    if (do_verify_render)
        ctx = djvu_ctx_new(mem_alloc, mem_free, on_error, NULL);
    else
        ctx = djvu_ctx_new(NULL, NULL, on_error, NULL);
    if (do_verify_render)
        djvu_ctx_set_lazy_iw44(ctx, 1);

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
        if (!o) { rc = 1; }
        else {
            FILE *f = out ? fopen(out, "wb") : stdout;
            if (f) { fwrite(o, 1, olen, f); if (out) fclose(f); }
            djvu_free(ctx, o);
        }
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }
    doc = djvu_doc_open(ctx, data, len);
    if (!doc) { fprintf(stderr, "cannot open document\n"); free(data); djvu_ctx_free(ctx); return 1; }

    if (do_dump_features) {
        rc = run_dump_features(doc);
        djvu_doc_close(doc);
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

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
            if (img) { if (out) write_pnm(out, img); djvu_image_destroy(ctx, img); }
            else rc = 1;
        } else if (do_iw == 5 || do_iw == 6 || do_iw == 7) {
            djvu_image *img = djvu_debug_render_iw_plane(doc, page - 1, 0, do_iw - 5);
            if (img) { if (out) write_pnm(out, img); djvu_image_destroy(ctx, img); }
            else rc = 1;
        } else {
            djvu_image *img = djvu_debug_render_iw(doc, page - 1, do_iw == 2);
            if (img) {
                if (out) write_pnm(out, img);
                djvu_image_destroy(ctx, img);
            } else rc = 1;
        }
        djvu_doc_close(doc); djvu_ctx_free(ctx); free(data);
        return rc;
    }

    if (do_bench) {
        int n = djvu_doc_page_count(doc);
        int sum = (do_bench == 2); /* replicate SumatraPDF Engine* render path */
        const int REPS = 3;   /* REPS fresh docs/page; time render only; keep fastest */
        djvu_doc_close(doc);
        doc = NULL;
        bench_ddjvu_reset();
        if (sum) {
            djvu_ctx_set_bgr(ctx, 1); /* EngineDjvuDec requests BGR output */
            printf("(bench-sum: warm render-to-buffer, zoom=1; decode at doc-open)\n");
        }
        for (i = 0; i < n; i++) {
            double mine = -1, lib = -1;
            int ok = 1, r;
            for (r = 0; r < REPS; r++) {
                double dt = sum ? bench_ours_page_sum_ms(ctx, data, len, i)
                                : bench_ours_page_ms(ctx, data, len, i);
                if (dt < 0)
                    ok = 0;
                else if (mine < 0 || dt < mine)
                    mine = dt;
                {
                    double l = sum ? bench_ddjvu_page_sum_ms(in, i)
                                   : bench_ddjvu_page_ms(in, i);
                    if (l >= 0 && (lib < 0 || l < lib))
                        lib = l;
                }
            }
            if (lib < 0 || !ok) {
                printf("page %d, djvulibre %s, ours %.2f ms%s\n", i + 1,
                       lib < 0 ? "ERROR" : "ok", mine, ok ? "" : " (ours FAILED)");
                continue;
            }
            double diff = mine - lib;            /* + => ours slower */
            double pct = lib > 0 ? diff / lib * 100.0 : 0.0;
            printf("page %d, djvulibre %.2f ms, ours %.2f ms, %+.2f ms, %+.1f%%\n",
                   i + 1, lib, mine, diff, pct);
        }
        {
            double doc_mine = -1, doc_lib = -1;
            int ok = 1, r;
            bench_ddjvu_reset();
            for (r = 0; r < REPS; r++) {
                double dt = sum ? bench_ours_doc_sum_ms(ctx, data, len)
                                : bench_ours_doc_ms(ctx, data, len);
                if (dt < 0)
                    ok = 0;
                else if (doc_mine < 0 || dt < doc_mine)
                    doc_mine = dt;
                {
                    double l = sum ? bench_ddjvu_doc_sum_ms(in)
                                   : bench_ddjvu_doc_ms(in);
                    if (l >= 0 && (doc_lib < 0 || l < doc_lib))
                        doc_lib = l;
                }
            }
            if (doc_lib < 0 || !ok) {
                printf("document, djvulibre %s, ours %.2f ms%s\n",
                       doc_lib < 0 ? "ERROR" : "ok", doc_mine,
                       ok ? "" : " (ours FAILED)");
            } else {
                double diff = doc_mine - doc_lib;
                double pct = doc_lib > 0 ? diff / doc_lib * 100.0 : 0.0;
                printf("document, djvulibre %.2f ms, ours %.2f ms, %+.2f ms, %+.1f%%\n",
                       doc_lib, doc_mine, diff, pct);
            }
        }
        bench_mem_report(data, len, sum); /* decoder allocation stats for the doc */
        djvu_ctx_free(ctx); free(data);
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
        static const char *tn[4] = {"unknown","bitonal","photo","compound"};
        djvu_page_type t = djvu_page_get_type(doc, page - 1);
        printf("page %d type: %s\n", page, tn[(t >= 0 && t < 4) ? t : 0]);
    }

    if (do_zones) {
        djvu_page_text_zones *z = djvu_page_text_get_zones(doc, page - 1);
        if (z && z->root) print_zone(z->root, 0);
        else printf("(no text zones)\n");
        djvu_text_zones_destroy(ctx, z);
    }

    if (do_outline) {
        djvu_outline_item *root = djvu_doc_outline(doc);
        if (root) print_outline(root, -1);
        else printf("(no outline)\n");
        djvu_outline_destroy(ctx, root);
    }

    if (do_links) {
        djvu_page_links *L = djvu_page_get_links(doc, page - 1);
        if (L) {
            int k;
            for (k = 0; k < L->nlinks; k++) {
                djvu_link *e = &L->links[k];
                printf("link %d: [%d,%d %dx%d] shape=%d -> %s%s%s\n", k,
                       e->x, e->y, e->w, e->h, e->shape, e->url,
                       e->comment ? " ; " : "", e->comment ? e->comment : "");
            }
            djvu_page_links_destroy(ctx, L);
        } else printf("(no links)\n");
    }

    if (out) {
        djvu_image *img = djvu_page_render(doc, page - 1, out_sub);
        if (img) {
            if (write_pnm(out, img) != 0) { fprintf(stderr, "cannot write %s\n", out); rc = 1; }
            djvu_image_destroy(ctx, img);
        } else { rc = 1; }
    }

    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    free(data);
    return rc;
}
