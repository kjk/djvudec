/* bench_ddjvu.cpp -- DjVuLibre oracle + timing shim for djvu_test.
 *
 * -bench times page decode+render with a fresh document per rep (open outside
 *   the timer), matching our djvu_page_render timing.
 * -verify-render uses the same ddjvuapi page_render for byte-exact compares. */
#include "libdjvu/ddjvuapi.h"
#include "miniexp.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>

extern "C" int djvu_test_ddjvu_cold;

/* Cached ddjvuapi document for -verify-render (bench opens fresh each rep). */
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
    if (!g_api_ctx) {
        g_api_ctx = ddjvu_context_create("djvu_test");
        if (g_api_ctx && djvu_test_ddjvu_cold)
            ddjvu_cache_set_size(g_api_ctx, 0);
    }
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

extern "C" {

void bench_ddjvu_reset(void)
{
    if (g_api_doc) {
        ddjvu_document_release(g_api_doc);
        g_api_doc = 0;
    }
    g_api_path[0] = 0;
}

/* Drop cached DjVuLibre decodes; release the API context (end of verify chunk). */
void bench_ddjvu_purge(void)
{
    bench_ddjvu_reset();
    if (!g_api_ctx)
        return;
    ddjvu_cache_clear(g_api_ctx);
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

/* Render one open page (decode + composite + rotation). Returns 0 on success. */
static int bench_ddjvu_render_open_page(ddjvu_page_t *page)
{
    ddjvu_format_t *fmt = 0;
    ddjvu_rect_t prect, rrect;
    ddjvu_format_style_t style;
    int iw, ih, rowsize, want_rgb;
    unsigned char *image = 0;
    int rc = -1;

    if (!page)
        return -1;
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

    want_rgb = (ddjvu_page_get_type(page) == DDJVU_PAGETYPE_BITONAL) ? 0 : 1;
    style = want_rgb ? DDJVU_FORMAT_RGB24 : DDJVU_FORMAT_GREY8;
    fmt = ddjvu_format_create(style, 0, 0);
    if (!fmt)
        goto done;
    ddjvu_format_set_row_order(fmt, 1);

    rowsize = want_rgb ? iw * 3 : iw;
    image = (unsigned char *)std::malloc((size_t)rowsize * (size_t)ih);
    if (!image)
        goto done;

    if (!ddjvu_page_render(page, DDJVU_RENDER_COLOR, &prect, &rrect, fmt,
                           rowsize, (char *)image))
        std::memset(image, 0xff, (size_t)rowsize * (size_t)ih);
    rc = 0;

done:
    std::free(image);
    if (fmt)
        ddjvu_format_release(fmt);
    return rc;
}

/* Cold page render: fresh doc per rep; timer covers page decode+render only. */
double bench_ddjvu_page_ms(const char *path, int page0)
{
    ddjvu_document_t *doc = 0;
    ddjvu_page_t *page = 0;
    double ms = -1.0;
    double t0;

    bench_ddjvu_reset();
    if (!g_api_ctx)
        g_api_ctx = ddjvu_context_create("djvu_test");
    if (!g_api_ctx)
        return -1.0;

    doc = ddjvu_document_create_by_filename_utf8(g_api_ctx, path, 1);
    if (!doc)
        goto done;
    while (!ddjvu_document_decoding_done(doc))
        api_handle(1);
    if (ddjvu_document_decoding_error(doc))
        goto done;

    page = ddjvu_page_create_by_pageno(doc, page0);
    if (!page)
        goto done;

    t0 = bench_now_ms();
    if (bench_ddjvu_render_open_page(page) == 0)
        ms = bench_now_ms() - t0;

done:
    if (page)
        ddjvu_page_release(page);
    if (doc)
        ddjvu_document_release(doc);
    return ms;
}

/* Time open + render all pages + text + annotations + close (one cold session). */
double bench_ddjvu_doc_ms(const char *path)
{
    ddjvu_document_t *doc = 0;
    double ms = -1.0;
    double t0;
    int np, p;

    bench_ddjvu_reset();
    if (!g_api_ctx)
        g_api_ctx = ddjvu_context_create("djvu_test");
    if (!g_api_ctx)
        return -1.0;

    t0 = bench_now_ms();

    doc = ddjvu_document_create_by_filename_utf8(g_api_ctx, path, 1);
    if (!doc)
        goto done;
    while (!ddjvu_document_decoding_done(doc))
        api_handle(1);
    if (ddjvu_document_decoding_error(doc))
        goto done;

    np = ddjvu_document_get_pagenum(doc);
    for (p = 0; p < np; p++) {
        ddjvu_page_t *page = ddjvu_page_create_by_pageno(doc, p);
        if (page) {
            (void)bench_ddjvu_render_open_page(page);
            ddjvu_page_release(page);
        }
        {
            miniexp_t r;
            while ((r = ddjvu_document_get_pagetext(doc, p, "page")) == miniexp_dummy)
                api_handle(1);
            (void)r;
        }
        {
            miniexp_t anno;
            while ((anno = ddjvu_document_get_pageanno(doc, p)) == miniexp_dummy)
                api_handle(1);
            if (miniexp_consp(anno)) {
                miniexp_t *links = ddjvu_anno_get_hyperlinks(anno);
                if (links)
                    std::free(links);
            }
        }
    }
    ms = bench_now_ms() - t0;

done:
    if (doc)
        ddjvu_document_release(doc);
    bench_ddjvu_reset();
    return ms;
}

/* --- bench-sum: replicate SumatraPDF EngineDjVu::RenderPage (libdjvu path) ---
 * Renders at zoom=1, user-rotation=0. Unlike bench_ddjvu_render_open_page (which
 * renders at native pixel size into RGB24), EngineDjVu renders into a BGR24
 * buffer at the *mediabox* size, i.e. the page scaled to fileDPI=300, and lets
 * ddjvu do the scaling during decode (prect=full, rrect=screen, here equal).
 * Per-page bench: page decode is warmed before the timer (mirrors our Sjbz cache
 * at doc-open). Document session bench still times decode on first touch. */

static int bench_ddjvu_sum_wait_page(ddjvu_page_t *page)
{
    if (!page)
        return -1;
    while (!ddjvu_page_decoding_done(page))
        api_handle(1);
    return ddjvu_page_decoding_error(page) ? -1 : 0;
}

static int bench_ddjvu_render_sum_page(ddjvu_page_t *page)
{
    ddjvu_format_t *fmt = 0;
    ddjvu_rect_t prect, rrect;
    ddjvu_format_style_t style;
    ddjvu_render_mode_t mode;
    int iw, ih, dpi, fullW, fullH, rowsize, want_rgb, bpp;
    unsigned char *image = 0;
    int rc = -1;

    if (!page)
        return -1;

    /* combine user rotation (0) with the page's intrinsic rotation */
    ddjvu_page_set_rotation(page, ddjvu_page_get_initial_rotation(page));

    iw = ddjvu_page_get_width(page);
    ih = ddjvu_page_get_height(page);
    dpi = ddjvu_page_get_resolution(page);
    if (iw <= 0 || ih <= 0)
        goto done;
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
    style = want_rgb ? DDJVU_FORMAT_BGR24 : DDJVU_FORMAT_GREY8;
    mode = want_rgb ? DDJVU_RENDER_COLOR : DDJVU_RENDER_MASKONLY;
    fmt = ddjvu_format_create(style, 0, 0);
    if (!fmt)
        goto done;
    ddjvu_format_set_row_order(fmt, 1);

    bpp = want_rgb ? 3 : 1;
    rowsize = ((fullW * bpp + 3) / 4) * 4; /* EngineDjVu DIB stride alignment */
    image = (unsigned char *)std::malloc((size_t)rowsize * (size_t)fullH);
    if (!image)
        goto done;

    if (!ddjvu_page_render(page, mode, &prect, &rrect, fmt, rowsize, (char *)image))
        std::memset(image, 0xff, (size_t)rowsize * (size_t)fullH);
    rc = 0;

done:
    std::free(image);
    if (fmt)
        ddjvu_format_release(fmt);
    return rc;
}

static int bench_ddjvu_render_sum_open_page(ddjvu_page_t *page)
{
    if (bench_ddjvu_sum_wait_page(page) != 0)
        return -1;
    return bench_ddjvu_render_sum_page(page);
}

/* Warm sum page render: fresh doc per rep; page decode before timer. */
double bench_ddjvu_page_sum_ms(const char *path, int page0)
{
    ddjvu_document_t *doc = 0;
    ddjvu_page_t *page = 0;
    double ms = -1.0;
    double t0;

    bench_ddjvu_reset();
    if (!g_api_ctx)
        g_api_ctx = ddjvu_context_create("djvu_test");
    if (!g_api_ctx)
        return -1.0;

    doc = ddjvu_document_create_by_filename_utf8(g_api_ctx, path, 1);
    if (!doc)
        goto done;
    while (!ddjvu_document_decoding_done(doc))
        api_handle(1);
    if (ddjvu_document_decoding_error(doc))
        goto done;

    page = ddjvu_page_create_by_pageno(doc, page0);
    if (!page)
        goto done;
    if (bench_ddjvu_sum_wait_page(page) != 0)
        goto done;

    t0 = bench_now_ms();
    if (bench_ddjvu_render_sum_page(page) == 0)
        ms = bench_now_ms() - t0;

done:
    if (page)
        ddjvu_page_release(page);
    if (doc)
        ddjvu_document_release(doc);
    return ms;
}

/* One cold session, sum render: open + render all pages + text + anno + close. */
double bench_ddjvu_doc_sum_ms(const char *path)
{
    ddjvu_document_t *doc = 0;
    double ms = -1.0;
    double t0;
    int np, p;

    bench_ddjvu_reset();
    if (!g_api_ctx)
        g_api_ctx = ddjvu_context_create("djvu_test");
    if (!g_api_ctx)
        return -1.0;

    t0 = bench_now_ms();

    doc = ddjvu_document_create_by_filename_utf8(g_api_ctx, path, 1);
    if (!doc)
        goto done;
    while (!ddjvu_document_decoding_done(doc))
        api_handle(1);
    if (ddjvu_document_decoding_error(doc))
        goto done;

    np = ddjvu_document_get_pagenum(doc);
    for (p = 0; p < np; p++) {
        ddjvu_page_t *page = ddjvu_page_create_by_pageno(doc, p);
        if (page) {
            (void)bench_ddjvu_render_sum_open_page(page);
            ddjvu_page_release(page);
        }
        {
            miniexp_t r;
            while ((r = ddjvu_document_get_pagetext(doc, p, "page")) == miniexp_dummy)
                api_handle(1);
            (void)r;
        }
        {
            miniexp_t anno;
            while ((anno = ddjvu_document_get_pageanno(doc, p)) == miniexp_dummy)
                api_handle(1);
            if (miniexp_consp(anno)) {
                miniexp_t *links = ddjvu_anno_get_hyperlinks(anno);
                if (links)
                    std::free(links);
            }
        }
    }
    ms = bench_now_ms() - t0;

done:
    if (doc)
        ddjvu_document_release(doc);
    bench_ddjvu_reset();
    return ms;
}

void bench_render_free(bench_render *img)
{
    if (img) {
        std::free(img->data);
        img->data = 0;
        img->width = img->height = img->bps = img->rowsize = 0;
    }
}

/* Render one decoded page into *out (ddjvu -format=pgm|ppm). */
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

/* Render page `page0` (0-based) via ddjvuapi (ddjvu -format=pgm|ppm).
 * want_rgb: 0 -> GREY8 (pgm), 1 -> RGB24 (ppm). Returns 0 on success.
 * Reuses one open document per path (bench_ddjvu_purge releases it at end of
 * a -verify-render chunk). DJVU_DDJVU_COLD disables the DjVuLibre file cache. */
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