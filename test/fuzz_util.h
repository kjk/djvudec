/* fuzz_util.h -- shared helpers for the libFuzzer targets (test/fuzz_*.c).
 *
 * - budgeted allocator: bounds live bytes per input so a crafted header that
 *   declares a gigapixel page fails through the library's alloc-failure paths
 *   (fuzzing those too) instead of tripping libFuzzer's RSS limit; also the
 *   per-input leak check (LeakSanitizer does not exist on Windows).
 * - fz_buf / fz_mkpage: build a minimal single-page FORM:DJVU in memory from
 *   raw chunk payloads, so the per-codec targets can drive the *public* render
 *   path (compose, scaler, rotation, text/links) on small synthesized pages.
 * - fz_take: split an input into length-prefixed fields; a mutated length
 *   just truncates to what is left (never rejects the input).
 * - fz_drive_page: the per-page public-API workout shared by every target.
 *
 * Header-only; each target is one translation unit. */
#ifndef FUZZ_UTIL_H
#define FUZZ_UTIL_H

#include "harness_util.h"
#include "djvu_internal.h"

#ifndef FZ_MEM_BUDGET
#define FZ_MEM_BUDGET ((size_t)1 << 30) /* 1 GB live per input */
#endif

/* Full-resolution renders are skipped above this many pixels per page (the
   subsampled render still runs). Full-res composite is the dominant cost per
   input and adds no codec coverage over the subsampled one on big pages.
   Per-input cost matters twice: for exec/s, and because libFuzzer replays
   the whole corpus at the start of every fuzz.ts time slice. */
#define FZ_MAX_FULLRES_PAGE_PIXELS ((unsigned long long)1 << 20)

static size_t fz_live;

HU_UNUSED static void *fz_alloc(void *user, void *ctx, size_t size)
{
    void *p;
    (void)user; (void)ctx;
    if (size > FZ_MEM_BUDGET - fz_live) return NULL;
    p = hu_size_malloc(size);
    if (p) fz_live += size;
    return p;
}

HU_UNUSED static void fz_free(void *user, void *ctx, void *ptr)
{
    (void)user; (void)ctx;
    fz_live -= hu_size_free(ptr);
}

/* Single-threaded harness: the lock hooks only have to exist (they enable
   per-page caching and lazy shared-dict decode, both worth fuzzing). */
HU_UNUSED static void fz_lock(void *user, void *ctx) { (void)user; (void)ctx; }
HU_UNUSED static void fz_unlock(void *user, void *ctx) { (void)user; (void)ctx; }

/* opts bits pick ctx options so every configuration gets fuzzed. */
enum {
    FZ_OPT_LOCKS = 1,     /* lock hooks: lazy shared-dict decode */
    FZ_OPT_CACHE = 2,     /* per-page layer cache (implies LOCKS) */
    FZ_OPT_BGR = 4,
    FZ_OPT_MAXCHUNKS = 8  /* decode only the first IW44 chunk per layer */
};

HU_UNUSED static djvu_ctx *fz_ctx_new(unsigned opts)
{
    djvu_ctx *ctx;
    int locks = (opts & (FZ_OPT_LOCKS | FZ_OPT_CACHE)) != 0;
    djvu_init();
    fz_live = 0;
    ctx = djvu_ctx_new(fz_alloc, fz_free, locks ? fz_lock : NULL,
                       locks ? fz_unlock : NULL, NULL, NULL);
    if (!ctx) return NULL;
    if (opts & FZ_OPT_CACHE) djvu_ctx_set_cache_per_page(ctx, 1);
    if (opts & FZ_OPT_BGR) djvu_ctx_set_bgr(ctx, 1);
    if (opts & FZ_OPT_MAXCHUNKS) djvu_ctx_set_iw_max_chunks(ctx, 1);
    return ctx;
}

HU_UNUSED static void fz_ctx_free(djvu_ctx *ctx)
{
    djvu_ctx_free(ctx);
    if (fz_live != 0) {
        fprintf(stderr, "fuzz: leaked %zu bytes\n", fz_live);
        abort();
    }
}

/* ---- growable byte buffer + IFF writer ---------------------------------- */

typedef struct { uint8_t *d; size_t n, cap; int oom; } fz_buf;

HU_UNUSED static void fz_put(fz_buf *b, const void *p, size_t n)
{
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        uint8_t *nd;
        while (nc < b->n + n) nc *= 2;
        nd = (uint8_t *)realloc(b->d, nc);
        if (!nd) { b->oom = 1; return; }
        b->d = nd; b->cap = nc;
    }
    if (n) memcpy(b->d + b->n, p, n);
    b->n += n;
}

HU_UNUSED static void fz_put_u8(fz_buf *b, unsigned v) { uint8_t c = (uint8_t)v; fz_put(b, &c, 1); }
HU_UNUSED static void fz_put_u16be(fz_buf *b, unsigned v) { fz_put_u8(b, v >> 8); fz_put_u8(b, v); }
HU_UNUSED static void fz_put_u32be(fz_buf *b, uint32_t v)
{
    fz_put_u16be(b, v >> 16); fz_put_u16be(b, v & 0xffff);
}

HU_UNUSED static void fz_set_u32be(fz_buf *b, size_t at, uint32_t v)
{
    if (b->oom || at + 4 > b->n) return;
    b->d[at] = (uint8_t)(v >> 24); b->d[at + 1] = (uint8_t)(v >> 16);
    b->d[at + 2] = (uint8_t)(v >> 8); b->d[at + 3] = (uint8_t)v;
}

HU_UNUSED static void fz_chunk(fz_buf *b, const char *id, const uint8_t *p, size_t n)
{
    fz_put(b, id, 4);
    fz_put_u32be(b, (uint32_t)n);
    fz_put(b, p, n);
    if (n & 1) fz_put_u8(b, 0);
}

/* Start "AT&TFORM<size>DJVU" + INFO(w,h,dpi). Returns the FORM size offset. */
HU_UNUSED static size_t fz_page_begin(fz_buf *b, int w, int h, int rot_flag)
{
    size_t sz_at;
    uint8_t info[10];
    fz_put(b, "AT&TFORM", 8);
    sz_at = b->n;
    fz_put_u32be(b, 0);
    fz_put(b, "DJVU", 4);
    info[0] = (uint8_t)(w >> 8); info[1] = (uint8_t)w;
    info[2] = (uint8_t)(h >> 8); info[3] = (uint8_t)h;
    info[4] = 24; info[5] = 0;          /* minor, major version */
    info[6] = 300 & 0xff; info[7] = 300 >> 8; /* dpi, LE */
    info[8] = 22;                       /* gamma 2.2 */
    info[9] = (uint8_t)(rot_flag & 7);
    fz_chunk(b, "INFO", info, sizeof(info));
    return sz_at;
}

HU_UNUSED static void fz_page_end(fz_buf *b, size_t sz_at)
{
    fz_set_u32be(b, sz_at, (uint32_t)(b->n - sz_at - 4));
}

/* ---- input splitting ----------------------------------------------------- */

typedef struct { const uint8_t *p; size_t n; } fz_in;

HU_UNUSED static unsigned fz_u8(fz_in *in)
{
    unsigned v;
    if (!in->n) return 0;
    v = in->p[0]; in->p++; in->n--;
    return v;
}

HU_UNUSED static uint32_t fz_u24(fz_in *in)
{
    uint32_t v = fz_u8(in) << 16;
    v |= fz_u8(in) << 8;
    return v | fz_u8(in);
}

/* Take a u24-length-prefixed field (clamped to what is left). */
HU_UNUSED static fz_in fz_take(fz_in *in)
{
    fz_in f;
    uint32_t len = fz_u24(in);
    if (len > in->n) len = (uint32_t)in->n;
    f.p = in->p; f.n = len;
    in->p += len; in->n -= len;
    return f;
}

/* ---- public-API workout for one page ------------------------------------- */

HU_UNUSED static void fz_render(djvu_doc *doc, djvu_ctx *ctx, int page, int sub, int into)
{
    if (into) {
        djvu_render_info ri;
        if (djvu_page_render_info(doc, page, sub, &ri) == 0 && ri.width > 0 &&
            ri.height > 0) {
            int bpp = ri.format == DJVU_FORMAT_RGB24 ? 3 : 1;
            size_t stride = (size_t)ri.width * bpp + 5; /* odd stride */
            size_t n = stride * (size_t)ri.height;
            if (n <= ((size_t)256 << 20)) {
                uint8_t *dst = (uint8_t *)malloc(n);
                if (dst) {
                    djvu_page_render_into(doc, page, sub, dst, (int)stride);
                    free(dst);
                }
            }
        }
    } else {
        djvu_image *img = djvu_page_render(doc, page, sub);
        if (img) djvu_image_destroy(ctx, img);
    }
}

/* sub <= 0 in fz_drive_page: pick the smallest subsample >= 2 whose output
   is at most this many pixels (real 300-600 dpi pages are 8-50 Mpx; a fixed
   subsample of 4 still composes millions of pixels per page). */
#define FZ_SUB_TARGET_PIXELS ((unsigned long long)512 << 10)

/* Full-res + subsampled render (render or render_into), text, zones, links.
   Full-res is skipped on pages above FZ_MAX_FULLRES_PAGE_PIXELS and once
   *fullres_budget (pixels, shared across a document) is spent. */
HU_UNUSED static void fz_drive_page(djvu_doc *doc, djvu_ctx *ctx, int page, int sub,
                          int into, unsigned long long *fullres_budget)
{
    djvu_page_info info;
    char *text;
    djvu_page_text_zones *zones;
    djvu_page_links *links;
    unsigned long long px = 0;

    /* page_info failure: render anyway (fuzzes that error path) */
    if (djvu_doc_page_info(doc, page, &info) == 0 && info.width > 0 &&
        info.height > 0)
        px = (unsigned long long)info.width * (unsigned long long)info.height;
    if (px <= FZ_MAX_FULLRES_PAGE_PIXELS && px <= *fullres_budget) {
        *fullres_budget -= px;
        fz_render(doc, ctx, page, 1, into);
    }
    if (sub <= 0)
        for (sub = 2; sub < 64 && px / ((unsigned long long)sub * sub) > FZ_SUB_TARGET_PIXELS; sub++) {}
    fz_render(doc, ctx, page, sub, into);

    text = djvu_page_text(doc, page);
    if (text) djvu_text_destroy(ctx, text);
    zones = djvu_page_text_get_zones(doc, page);
    if (zones) djvu_text_zones_destroy(ctx, zones);
    links = djvu_page_get_links(doc, page);
    if (links) djvu_page_links_destroy(ctx, links);
    (void)djvu_page_get_type(doc, page);
}

/* Open a synthesized single-page document and drive it; full-res render
   only up to fullres_cap pixels. */
HU_UNUSED static void fz_drive_buf(djvu_ctx *ctx, const fz_buf *b, int sub, int into,
                                   unsigned long long fullres_cap)
{
    djvu_doc *doc;
    unsigned long long budget = fullres_cap;
    if (b->oom || !b->n) return;
    doc = djvu_doc_open(ctx, b->d, b->n);
    if (!doc) return;
    if (djvu_doc_page_count(doc) > 0) fz_drive_page(doc, ctx, 0, sub, into, &budget);
    djvu_doc_close(doc);
}

#endif /* FUZZ_UTIL_H */
