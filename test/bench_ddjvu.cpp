/* bench_ddjvu.cpp -- DjVuLibre oracle + timing shim for djvu_test.
 *
 * -bench / -bench-sum: bench_ddjvu_session opens the document, renders every
 *   page and closes it, timing each step with the same clock djvu_test uses for
 *   our side (bench_now_ms). Every session decodes cold (bench_ddjvu_reset).
 * -verify-render: bench_ddjvu_render_page returns the ddjvuapi page_render
 *   pixels for byte-exact compares. */
#include "libdjvu/ddjvuapi.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>

/* Cached ddjvuapi document for -verify-render (bench opens a fresh one per
   session). */
static ddjvu_context_t *g_api_ctx;
static ddjvu_document_t *g_api_doc;
static char g_api_path[4096];

static void api_handle(int wait)
{
    const ddjvu_message_t *msg;
    if (!g_api_ctx)
        return;
    if (wait)
        (void)ddjvu_message_wait(g_api_ctx);
    while ((msg = ddjvu_message_peek(g_api_ctx))) {
        ddjvu_message_pop(g_api_ctx);
    }
}

static int api_open_doc(const char *path)
{
    if (!g_api_ctx)
        g_api_ctx = ddjvu_context_create("djvu_test");
    if (!g_api_ctx)
        return -1;
    if (!g_api_doc || std::strcmp(g_api_path, path) != 0) {
        if (g_api_doc) {
            ddjvu_document_release(g_api_doc);
            g_api_doc = 0;
        }
        g_api_doc = ddjvu_document_create_by_filename_utf8(g_api_ctx, path, 1);
        if (!g_api_doc)
            return -1;
        while (!ddjvu_document_decoding_done(g_api_doc))
            api_handle(1);
        if (ddjvu_document_decoding_error(g_api_doc))
            return -1;
        std::strncpy(g_api_path, path, sizeof(g_api_path) - 1);
        g_api_path[sizeof(g_api_path) - 1] = 0;
    }
    return 0;
}

typedef struct bench_render {
    int width, height;
    int bps;
    int rowsize;
    unsigned char *data;
} bench_render;

typedef struct bench_session_timings {
    double open_ms;
    double close_ms;
    double total_ms;
    double *page_ms;
    int npages;
} bench_session_timings;

extern "C" {

void bench_ddjvu_reset(void)
{
    if (g_api_doc) {
        ddjvu_document_release(g_api_doc);
        g_api_doc = 0;
    }
    g_api_path[0] = 0;
    /* Drop decoded DjVuFiles retained by the CONTEXT-level cache. Without this
       a re-opened document gets its page decodes from the cache (~0 ms), so a
       best-of-2 bench compared our cold decode against a cache-warm libdjvu.
       (ddjvu_cache_set_size(ctx, 0) is a no-op: the API ignores sizes <= 0.) */
    if (g_api_ctx)
        ddjvu_cache_clear(g_api_ctx);
}

/* Drop cached DjVuLibre decodes (bench_ddjvu_reset clears the cache) and
   release the API context (end of a -verify-render chunk). */
void bench_ddjvu_purge(void)
{
    bench_ddjvu_reset();
    if (!g_api_ctx)
        return;
    ddjvu_context_release(g_api_ctx);
    g_api_ctx = 0;
}

void bench_ddjvu_mem_debug(FILE *out)
{
    if (!out)
        return;
    fprintf(out, "\tddjvu_ctx=%d ddjvu_doc_open=%d",
            g_api_ctx ? 1 : 0, g_api_doc ? 1 : 0);
    if (g_api_ctx)
        fprintf(out, " ddjvu_cache_max=%lu", ddjvu_cache_get_size(g_api_ctx));
    if (g_api_doc)
        fprintf(out, " ddjvu_npages=%d", ddjvu_document_get_pagenum(g_api_doc));
}

/* Monotonic high-resolution timestamp in milliseconds (same clock both sides). */
double bench_now_ms(void)
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* -bench: wait for the page decode, then render it at native size (decode +
   composite + rotation) into a freshly malloc'd buffer, like our
   djvu_page_render which also allocates its result inside the timed call.
   On success returns 0 and hands the buffer to the caller in *out_image; the
   caller frees it AFTER stopping the clock (our side destroys its djvu_image
   outside the timer too). */
static int bench_ddjvu_render_open_page(ddjvu_page_t *page, unsigned char **out_image)
{
    ddjvu_format_t *fmt = 0;
    ddjvu_rect_t prect, rrect;
    ddjvu_format_style_t style;
    int iw, ih, rowsize, want_rgb;
    unsigned char *image = 0;

    *out_image = 0;
    if (!page)
        return -1;
    while (!ddjvu_page_decoding_done(page))
        api_handle(1);
    if (ddjvu_page_decoding_error(page))
        return -1;

    iw = ddjvu_page_get_width(page);
    ih = ddjvu_page_get_height(page);
    if (iw <= 0 || ih <= 0)
        return -1;

    prect.x = prect.y = 0;
    prect.w = (unsigned int)iw;
    prect.h = (unsigned int)ih;
    rrect = prect;

    want_rgb = (ddjvu_page_get_type(page) == DDJVU_PAGETYPE_BITONAL) ? 0 : 1;
    style = want_rgb ? DDJVU_FORMAT_RGB24 : DDJVU_FORMAT_GREY8;
    fmt = ddjvu_format_create(style, 0, 0);
    if (!fmt)
        return -1;
    ddjvu_format_set_row_order(fmt, 1);

    rowsize = want_rgb ? iw * 3 : iw;
    image = (unsigned char *)std::malloc((size_t)rowsize * (size_t)ih);
    if (!image) {
        ddjvu_format_release(fmt);
        return -1;
    }

    if (!ddjvu_page_render(page, DDJVU_RENDER_COLOR, &prect, &rrect, fmt,
                           rowsize, (char *)image))
        std::memset(image, 0xff, (size_t)rowsize * (size_t)ih);
    ddjvu_format_release(fmt);
    *out_image = image;
    return 0;
}

/* --- bench-sum: replicate SumatraPDF EngineDjVu::RenderPage (libdjvu path) ---
 * Renders at zoom=1, user-rotation=0. Unlike bench_ddjvu_render_open_page (which
 * renders at native pixel size into RGB24), EngineDjVu renders into a BGR24
 * buffer at the *mediabox* size, i.e. the page scaled to fileDPI=300, and lets
 * ddjvu do the scaling during decode (prect=full, rrect=screen, here equal).
 * The destination buffer stands in for the engine's DIB: the caller allocates
 * and touches it before starting the clock and frees it after stopping it,
 * exactly like djvu_test does for our side. */

/* DIB row size for a w-pixel row at bpp bytes/pixel (EngineDjVu alignment). */
static size_t sum_rowsize(int w, int bpp)
{
    return (((size_t)w * (size_t)bpp + 3) / 4) * 4;
}

/* Worst-case destination size for a -bench-sum render of page p, computed from
   the INFO chunk only (ddjvu_document_get_pageinfo does not decode the page,
   so this can run before the timer): BGR24 at the mediabox size, in whichever
   orientation needs more row padding. Returns 0 on failure. */
static size_t bench_ddjvu_sum_capacity(ddjvu_document_t *doc, int p)
{
    ddjvu_pageinfo_t info;
    ddjvu_status_t st;
    int dpi, fullW, fullH;
    size_t a, b;

    while ((st = ddjvu_document_get_pageinfo(doc, p, &info)) < DDJVU_JOB_OK)
        api_handle(1);
    if (st != DDJVU_JOB_OK || info.width <= 0 || info.height <= 0)
        return 0;
    dpi = info.dpi;
    if (dpi < 25 || dpi > 6000)
        dpi = 300;
    fullW = (int)(300.0 * info.width / dpi + 0.5);
    fullH = (int)(300.0 * info.height / dpi + 0.5);
    if (fullW < 1)
        fullW = 1;
    if (fullH < 1)
        fullH = 1;
    a = sum_rowsize(fullW, 3) * (size_t)fullH;
    b = sum_rowsize(fullH, 3) * (size_t)fullW; /* 90/270 rotation swaps dims */
    return a > b ? a : b;
}

/* Wait for the page decode, then render into the caller's buffer (cap bytes). */
static int bench_ddjvu_render_sum_page(ddjvu_page_t *page, unsigned char *image,
                                       size_t cap)
{
    ddjvu_format_t *fmt = 0;
    ddjvu_rect_t prect, rrect;
    ddjvu_format_style_t style;
    ddjvu_render_mode_t mode;
    int iw, ih, dpi, fullW, fullH, want_rgb;
    size_t rowsize;

    if (!page || !image)
        return -1;
    while (!ddjvu_page_decoding_done(page))
        api_handle(1);
    if (ddjvu_page_decoding_error(page))
        return -1;

    /* combine user rotation (0) with the page's intrinsic rotation */
    ddjvu_page_set_rotation(page, ddjvu_page_get_initial_rotation(page));

    iw = ddjvu_page_get_width(page);
    ih = ddjvu_page_get_height(page);
    dpi = ddjvu_page_get_resolution(page);
    if (iw <= 0 || ih <= 0)
        return -1;
    if (dpi < 25 || dpi > 6000)
        dpi = 300;
    /* mediabox size = page scaled to fileDPI (300), as EngineDjVu computes it */
    fullW = (int)(300.0 * iw / dpi + 0.5);
    fullH = (int)(300.0 * ih / dpi + 0.5);
    if (fullW < 1)
        fullW = 1;
    if (fullH < 1)
        fullH = 1;

    prect.x = prect.y = 0;
    prect.w = (unsigned int)fullW;
    prect.h = (unsigned int)fullH;
    rrect = prect; /* screen == full at zoom 1, rotation 0 */

    want_rgb = (ddjvu_page_get_type(page) == DDJVU_PAGETYPE_BITONAL) ? 0 : 1;
    rowsize = sum_rowsize(fullW, want_rgb ? 3 : 1);
    if (rowsize * (size_t)fullH > cap)
        return -1;

    style = want_rgb ? DDJVU_FORMAT_BGR24 : DDJVU_FORMAT_GREY8;
    mode = want_rgb ? DDJVU_RENDER_COLOR : DDJVU_RENDER_MASKONLY;
    fmt = ddjvu_format_create(style, 0, 0);
    if (!fmt)
        return -1;
    ddjvu_format_set_row_order(fmt, 1);

    if (!ddjvu_page_render(page, mode, &prect, &rrect, fmt, (unsigned long)rowsize,
                           (char *)image))
        std::memset(image, 0xff, rowsize * (size_t)fullH);
    ddjvu_format_release(fmt);
    return 0;
}

void bench_render_free(bench_render *img)
{
    if (img) {
        std::free(img->data);
        img->data = 0;
        img->width = img->height = img->bps = img->rowsize = 0;
    }
}

/* One session: open doc, render every page, close. Fills page_ms[0..npages-1].
   The per-page clock covers page create + decode + render + page release.
   What the rendered pixels live in is kept symmetric with djvu_test's
   bench_ours_session:
     sum == 0: the render allocates its result inside the timer (as our
               djvu_page_render does); it is freed after the clock stops.
     sum != 0: the destination buffer is allocated and touched (memset) before
               the clock starts and freed after it stops.
   total_ms is the sum of the timed regions (open + pages + close). */
int bench_ddjvu_session(const char *path, int sum, bench_session_timings *out)
{
    ddjvu_document_t *doc = 0;
    int np, p;
    double t0;

    if (!out || !out->page_ms || out->npages <= 0)
        return -1;

    bench_ddjvu_reset();
    if (!g_api_ctx)
        g_api_ctx = ddjvu_context_create("djvu_test");
    if (!g_api_ctx)
        return -1;

    t0 = bench_now_ms();
    doc = ddjvu_document_create_by_filename_utf8(g_api_ctx, path, 1);
    if (!doc)
        return -1;
    while (!ddjvu_document_decoding_done(doc))
        api_handle(1);
    if (ddjvu_document_decoding_error(doc)) {
        ddjvu_document_release(doc);
        bench_ddjvu_reset();
        return -1;
    }
    out->open_ms = bench_now_ms() - t0;

    np = ddjvu_document_get_pagenum(doc);
    if (np > out->npages)
        np = out->npages;

    for (p = 0; p < np; p++) {
        ddjvu_page_t *page = 0;
        unsigned char *image = 0;
        size_t cap = 0;
        int ok = -1;

        if (sum) {
            cap = bench_ddjvu_sum_capacity(doc, p);
            image = cap ? (unsigned char *)std::malloc(cap) : 0;
            if (!image) {
                out->page_ms[p] = -1.0;
                continue;
            }
            std::memset(image, 0xff, cap);
        }

        t0 = bench_now_ms();
        page = ddjvu_page_create_by_pageno(doc, p);
        if (page) {
            if (sum)
                ok = bench_ddjvu_render_sum_page(page, image, cap);
            else
                ok = bench_ddjvu_render_open_page(page, &image);
            ddjvu_page_release(page);
        }
        out->page_ms[p] = bench_now_ms() - t0;
        std::free(image);
        if (ok != 0)
            out->page_ms[p] = -1.0;
    }
    for (p = np; p < out->npages; p++)
        out->page_ms[p] = -1.0;

    t0 = bench_now_ms();
    ddjvu_document_release(doc);
    out->close_ms = bench_now_ms() - t0;
    /* total = the timed regions only (open + rendered pages + close), as on
       our side: the untimed buffer handling and cache reset stay out of it */
    out->total_ms = out->open_ms + out->close_ms;
    for (p = 0; p < np; p++)
        if (out->page_ms[p] >= 0.0)
            out->total_ms += out->page_ms[p];
    bench_ddjvu_reset();
    return 0;
}

/* Render one page at native size into *out (the caller owns out->data). */
static int bench_ddjvu_render_open_page_out(ddjvu_page_t *page, int want_rgb,
                                            bench_render *out)
{
    ddjvu_format_t *fmt = 0;
    ddjvu_rect_t prect, rrect;
    ddjvu_format_style_t style;
    int iw, ih, rowsize;
    unsigned char *image = 0;
    int rc = -1;

    if (!out || !page)
        return -1;
    out->width = out->height = out->bps = out->rowsize = 0;
    out->data = 0;

    while (!ddjvu_page_decoding_done(page))
        api_handle(1);
    if (ddjvu_page_decoding_error(page))
        goto done;

    iw = ddjvu_page_get_width(page);
    ih = ddjvu_page_get_height(page);
    if (iw <= 0 || ih <= 0)
        goto done;
    prect.x = prect.y = 0;
    prect.w = (unsigned int)iw;
    prect.h = (unsigned int)ih;
    rrect = prect;

    /* BGR24 is DjVuLibre's native pixel order (its GPixmap is B,G,R), so this is
       a memcpy in fmt_convert; RGB24 would cost a per-pixel swap. djvu_test
       renders BGR too (djvu_ctx_set_bgr) and compares BGR<->BGR. */
    style = want_rgb ? DDJVU_FORMAT_BGR24 : DDJVU_FORMAT_GREY8;
    fmt = ddjvu_format_create(style, 0, 0);
    if (!fmt)
        goto done;
    ddjvu_format_set_row_order(fmt, 1);

    out->bps = want_rgb ? 3 : 1;
    rowsize = want_rgb ? iw * 3 : iw;
    image = (unsigned char *)std::malloc((size_t)rowsize * (size_t)ih);
    if (!image)
        goto done;

    if (!ddjvu_page_render(page, DDJVU_RENDER_COLOR, &prect, &rrect, fmt,
                           rowsize, (char *)image))
        std::memset(image, 0xff, (size_t)rowsize * (size_t)ih);

    out->width = iw;
    out->height = ih;
    out->rowsize = rowsize;
    out->data = image;
    image = 0;
    rc = 0;

done:
    std::free(image);
    if (fmt)
        ddjvu_format_release(fmt);
    return rc;
}

/* Render page `page0` (0-based) via ddjvuapi, like `ddjvu -format=pgm|ppm`.
 * want_rgb: 0 -> GREY8, 1 -> BGR24 (3 bytes/pixel in B,G,R order; see
 * bench_ddjvu_render_open_page_out). Returns 0 on success; free the result
 * with bench_render_free. Reuses one open document per path, with DjVuLibre's
 * default context cache; bench_ddjvu_purge releases both at the end of a
 * -verify-render chunk. */
int bench_ddjvu_render_page(const char *path, int page0, int want_rgb,
                            bench_render *out)
{
    ddjvu_page_t *page = 0;
    int rc = -1;

    if (!out)
        return -1;
    out->width = out->height = out->bps = out->rowsize = 0;
    out->data = 0;

    if (api_open_doc(path) != 0)
        return -1;

    page = ddjvu_page_create_by_pageno(g_api_doc, page0);
    if (!page)
        return -1;
    rc = bench_ddjvu_render_open_page_out(page, want_rgb, out);

    if (page)
        ddjvu_page_release(page);
    return rc;
}

} /* extern "C" */