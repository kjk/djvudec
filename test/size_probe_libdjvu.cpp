/* size_probe_libdjvu.cpp -- minimal ddjvuapi driver for code-size comparison.
 *
 * Exercises the same viewer-facing paths as size_probe_djvudec.c: page render,
 * hidden text, hyperlinks, and document outline. */
#include "libdjvu/ddjvuapi.h"
#include "miniexp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static volatile unsigned long g_sink;

static void handle_messages(ddjvu_context_t *ctx, int wait)
{
    const ddjvu_message_t *msg;

    if (!ctx)
        return;
    if (wait)
        (void)ddjvu_message_wait(ctx);
    while ((msg = ddjvu_message_peek(ctx))) {
        if (msg->m_any.tag == DDJVU_ERROR)
            g_sink++;
        ddjvu_message_pop(ctx);
    }
}

static void touch_miniexp(miniexp_t exp)
{
    if (exp == miniexp_dummy || exp == miniexp_nil)
        return;
    if (miniexp_stringp(exp)) {
        const char *s = miniexp_to_str(exp);
        if (s && s[0])
            g_sink += (unsigned char)s[0];
        return;
    }
    if (miniexp_numberp(exp)) {
        g_sink += (unsigned long)miniexp_to_int(exp);
        return;
    }
    if (miniexp_consp(exp)) {
        miniexp_t car = miniexp_car(exp);
        miniexp_t cdr = miniexp_cdr(exp);
        touch_miniexp(car);
        touch_miniexp(cdr);
    }
}

static int render_page(ddjvu_context_t *ctx, ddjvu_page_t *page)
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
        handle_messages(ctx, 1);
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

    for (int y = 0; y < ih; y += 17) {
        for (int x = 0; x < iw; x += 13)
            g_sink += image[(size_t)y * (size_t)rowsize + (size_t)x * (want_rgb ? 3 : 1)];
    }
    rc = 0;

done:
    std::free(image);
    if (fmt)
        ddjvu_format_release(fmt);
    return rc;
}

int main(int argc, char **argv)
{
    const char *path;
    ddjvu_context_t *ctx;
    ddjvu_document_t *doc;
    miniexp_t outline;
    int np, p;

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s file.djvu\n", argv[0]);
        return 2;
    }
    path = argv[1];

    ctx = ddjvu_context_create("size_probe_libdjvu");
    if (!ctx)
        return 1;

    doc = ddjvu_document_create_by_filename_utf8(ctx, path, 1);
    if (!doc) {
        ddjvu_context_release(ctx);
        return 1;
    }
    while (!ddjvu_document_decoding_done(doc))
        handle_messages(ctx, 1);
    if (ddjvu_document_decoding_error(doc)) {
        ddjvu_document_release(doc);
        ddjvu_context_release(ctx);
        return 1;
    }

    np = ddjvu_document_get_pagenum(doc);
    g_sink += (unsigned long)np;

    outline = ddjvu_document_get_outline(doc);
    while (outline == miniexp_dummy) {
        handle_messages(ctx, 1);
        outline = ddjvu_document_get_outline(doc);
    }
    touch_miniexp(outline);

    for (p = 0; p < np; p++) {
        ddjvu_page_t *page;
        miniexp_t text;
        miniexp_t anno;

        page = ddjvu_page_create_by_pageno(doc, p);
        if (page) {
            g_sink += (unsigned long)ddjvu_page_get_type(page);
            (void)render_page(ctx, page);
            ddjvu_page_release(page);
        }

        text = ddjvu_document_get_pagetext(doc, p, "page");
        while (text == miniexp_dummy) {
            handle_messages(ctx, 1);
            text = ddjvu_document_get_pagetext(doc, p, "page");
        }
        touch_miniexp(text);

        anno = ddjvu_document_get_pageanno(doc, p);
        while (anno == miniexp_dummy) {
            handle_messages(ctx, 1);
            anno = ddjvu_document_get_pageanno(doc, p);
        }
        if (miniexp_consp(anno)) {
            miniexp_t *links = ddjvu_anno_get_hyperlinks(anno);
            if (links) {
                for (int i = 0; links[i] != miniexp_nil; i++)
                    touch_miniexp(links[i]);
                std::free(links);
            }
        }
    }

    ddjvu_document_release(doc);
    ddjvu_context_release(ctx);

    std::printf("%lu\n", g_sink);
    return 0;
}