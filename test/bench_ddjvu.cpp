/* bench_ddjvu.cpp -- DjVuLibre oracle + timing shim for djvu_test.
 *
 * -bench uses DjVuDocument/DjVuImage (same high-level composite path).
 * -verify-render uses ddjvuapi page_render (same as ddjvu.exe PNM output). */
#include "libdjvu/DjVuDocument.h"
#include "libdjvu/DjVuImage.h"
#include "libdjvu/GPixmap.h"
#include "libdjvu/GBitmap.h"
#include "libdjvu/GRect.h"
#include "libdjvu/GURL.h"
#include "libdjvu/GException.h"
#include "libdjvu/ddjvuapi.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>

using namespace DJVU;

/* Cached DjVuLibre document for repeated page renders (-bench). */
static GP<DjVuDocument> g_bench_doc;
static char g_bench_path[4096];

static GP<DjVuDocument> bench_open_doc(const char *path)
{
    if (!g_bench_doc || std::strcmp(g_bench_path, path) != 0) {
        GURL url = GURL::Filename::UTF8(path);
        g_bench_doc = DjVuDocument::create_wait(url);
        if (!g_bench_doc)
            return 0;
        std::strncpy(g_bench_path, path, sizeof(g_bench_path) - 1);
        g_bench_path[sizeof(g_bench_path) - 1] = 0;
    }
    return g_bench_doc;
}

/* Cached ddjvuapi document for -verify-render (matches ddjvu.exe). */
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

extern "C" {

void bench_ddjvu_reset(void)
{
    g_bench_doc = 0;
    g_bench_path[0] = 0;
    if (g_api_doc) {
        ddjvu_document_release(g_api_doc);
        g_api_doc = 0;
    }
    g_api_path[0] = 0;
}

/* Monotonic high-resolution timestamp in milliseconds (same clock both sides). */
double bench_now_ms(void)
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double bench_ddjvu_page_ms(const char *path, int page0)
{
    double ms = -1.0;
    G_TRY
    {
        GP<DjVuDocument> doc = bench_open_doc(path);
        if (doc) {
            double t0 = bench_now_ms();
            GP<DjVuImage> dimg = doc->get_page(page0, true);
            if (dimg) {
                GRect rect(0, 0, dimg->get_width(), dimg->get_height());
                if (dimg->is_legal_bilevel())
                    (void)dimg->get_bitmap(rect, 1);
                else
                    (void)dimg->get_pixmap(rect, 1, 0);
                ms = bench_now_ms() - t0;
            }
        }
    }
    G_CATCH(ex)
    {
        (void)ex;
        ms = -1.0;
    }
    G_ENDCATCH;
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

/* Render page `page0` (0-based) via ddjvuapi (ddjvu -format=pgm|ppm).
 * want_rgb: 0 -> GREY8 (pgm), 1 -> RGB24 (ppm). Returns 0 on success. */
int bench_ddjvu_render_page(const char *path, int page0, int want_rgb,
                            bench_render *out)
{
    ddjvu_page_t *page = 0;
    ddjvu_format_t *fmt = 0;
    ddjvu_rect_t prect, rrect;
    ddjvu_format_style_t style;
    int iw, ih, rowsize;
    unsigned char *image = 0;
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
    while (!ddjvu_page_decoding_done(page))
        api_handle(1);
    if (ddjvu_page_decoding_error(page))
        goto done;

    iw = ddjvu_page_get_width(page);
    ih = ddjvu_page_get_height(page);
    prect.x = prect.y = 0;
    prect.w = (unsigned int)iw;
    prect.h = (unsigned int)ih;
    rrect = prect;

    style = want_rgb ? DDJVU_FORMAT_RGB24 : DDJVU_FORMAT_GREY8;
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
    if (page)
        ddjvu_page_release(page);
    return rc;
}

} /* extern "C" */