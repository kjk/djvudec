/* fuzz_ddjvu.cpp -- in-memory DjVuLibre (ddjvuapi) oracle for fuzz_diff.c.
 *
 * Like bench_ddjvu.cpp's -verify-render path, but the document comes from a
 * memory buffer (stream 0) instead of a file, and every input gets a fresh
 * document with the context cache cleared, so one input's state can't leak
 * into the next. Compiled uninstrumented (it is the oracle, not the code
 * under test) and linked with the prebuilt libdjvu. */
#include "libdjvu/ddjvuapi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef struct bench_render {
    int width, height;
    int bps;
    int rowsize;
    unsigned char *data;
} bench_render;

static ddjvu_context_t *g_ctx;
static ddjvu_document_t *g_doc;
static int g_errors; /* DDJVU_ERROR messages since fz_ddjvu_open */

/* Drain messages; any request for a stream other than 0 (indirect
   components) is refused -- the whole document is the one buffer. */
static void handle(int wait)
{
    const ddjvu_message_t *msg;
    if (wait) (void)ddjvu_message_wait(g_ctx);
    while ((msg = ddjvu_message_peek(g_ctx))) {
        if (msg->m_any.tag == DDJVU_NEWSTREAM && msg->m_newstream.streamid != 0 && g_doc)
            ddjvu_stream_close(g_doc, msg->m_newstream.streamid, 1);
        if (msg->m_any.tag == DDJVU_ERROR) g_errors++;
        /* FUZZ_DIFF_VERBOSE=1: show DjVuLibre's diagnostics (triage) */
        if (getenv("FUZZ_DIFF_VERBOSE")) {
            if (msg->m_any.tag == DDJVU_ERROR)
                std::fprintf(stderr, "ddjvu error: %s\n", msg->m_error.message);
            else if (msg->m_any.tag == DDJVU_INFO)
                std::fprintf(stderr, "ddjvu info: %s\n", msg->m_info.message);
        }
        ddjvu_message_pop(g_ctx);
    }
}

extern "C" {

void fz_ddjvu_close(void)
{
    if (g_doc) {
        ddjvu_document_release(g_doc);
        g_doc = 0;
    }
    if (g_ctx) {
        handle(0);
        ddjvu_cache_clear(g_ctx);
    }
}

/* Open data as a document. Returns the page count, or -1 on failure. */
int fz_ddjvu_open(const unsigned char *data, size_t len)
{
    fz_ddjvu_close();
    g_errors = 0;
    if (!g_ctx) g_ctx = ddjvu_context_create("djvudec_fuzz_diff");
    if (!g_ctx) return -1;
    g_doc = ddjvu_document_create(g_ctx, 0, 0);
    if (!g_doc) return -1;
    ddjvu_stream_write(g_doc, 0, (const char *)data, (unsigned long)len);
    ddjvu_stream_close(g_doc, 0, 0);
    while (!ddjvu_document_decoding_done(g_doc)) handle(1);
    handle(0);
    if (ddjvu_document_decoding_error(g_doc)) return -1;
    return ddjvu_document_get_pagenum(g_doc);
}

/* Errors DjVuLibre reported (DDJVU_ERROR) since fz_ddjvu_open. */
int fz_ddjvu_errors(void)
{
    if (g_ctx) handle(0);
    return g_errors;
}

void bench_render_free(bench_render *img)
{
    if (img) {
        std::free(img->data);
        img->data = 0;
        img->width = img->height = img->bps = img->rowsize = 0;
    }
}

/* Render page0 at full resolution, GREY8 or BGR24 (DjVuLibre's native
   order; the fuzz target renders BGR too). 0 on success. */
int fz_ddjvu_render(int page0, int want_rgb, bench_render *out)
{
    ddjvu_page_t *page;
    ddjvu_format_t *fmt;
    ddjvu_rect_t prect;
    int iw, ih, rowsize, rc = -1;
    unsigned char *image;

    std::memset(out, 0, sizeof(*out));
    if (!g_doc) return -1;
    page = ddjvu_page_create_by_pageno(g_doc, page0);
    if (!page) return -1;
    while (!ddjvu_page_decoding_done(page)) handle(1);
    handle(0);
    iw = ddjvu_page_get_width(page);
    ih = ddjvu_page_get_height(page);
    if (ddjvu_page_decoding_error(page) || iw <= 0 || ih <= 0) {
        ddjvu_page_release(page);
        return -1;
    }
    prect.x = prect.y = 0;
    prect.w = (unsigned)iw;
    prect.h = (unsigned)ih;
    fmt = ddjvu_format_create(want_rgb ? DDJVU_FORMAT_BGR24 : DDJVU_FORMAT_GREY8, 0, 0);
    if (fmt) {
        ddjvu_format_set_row_order(fmt, 1);
        rowsize = want_rgb ? iw * 3 : iw;
        image = (unsigned char *)std::malloc((size_t)rowsize * (size_t)ih);
        if (image) {
            if (ddjvu_page_render(page, DDJVU_RENDER_COLOR, &prect, &prect, fmt,
                                  rowsize, (char *)image)) {
                out->width = iw;
                out->height = ih;
                out->bps = want_rgb ? 3 : 1;
                out->rowsize = rowsize;
                out->data = image;
                rc = 0;
            } else {
                std::free(image);
            }
        }
        ddjvu_format_release(fmt);
    }
    ddjvu_page_release(page);
    return rc;
}

} /* extern "C" */
