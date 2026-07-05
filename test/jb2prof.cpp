/* jb2prof.cpp -- time DjVuLibre's JB2 phases on raw chunks (perf oracle).
 *   jb2prof dict.djbz page.sjbz [reps]              raw JB2Image phases
 *   jb2prof -ddjvu file.djvu <page0> [reps]         ddjvuapi create+decode+render
 * Prints best-of-reps ms per phase.
 */
#include "libdjvu/JB2Image.h"
#include "libdjvu/ByteStream.h"
#include "libdjvu/GBitmap.h"
#include "libdjvu/GURL.h"
#include "libdjvu/ddjvuapi.h"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using namespace DJVU;

static double now_ms()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* JB2DecoderCallback: hand the pre-decoded dict to the page decoder. */
static GP<JB2Dict> dict_cb(void *arg)
{
    return *(GP<JB2Dict> *)arg;
}

static int run_ddjvu(const char *path, int page0, int reps)
{
    ddjvu_context_t *ctx = ddjvu_context_create("jb2prof");
    double best_create = 1e9, best_render = 1e9;
    if (!ctx) return 1;
    for (int r = 0; r < reps; r++) {
        ddjvu_document_t *doc =
            ddjvu_document_create_by_filename_utf8(ctx, path, 1);
        if (!doc) return 1;
        while (!ddjvu_document_decoding_done(doc)) {
            ddjvu_message_wait(ctx);
            while (ddjvu_message_peek(ctx)) ddjvu_message_pop(ctx);
        }
        double t0 = now_ms();
        ddjvu_page_t *page = ddjvu_page_create_by_pageno(doc, page0);
        while (!ddjvu_page_decoding_done(page)) {
            ddjvu_message_wait(ctx);
            while (ddjvu_message_peek(ctx)) ddjvu_message_pop(ctx);
        }
        double t1 = now_ms();
        if (t1 - t0 < best_create) best_create = t1 - t0;

        int iw = ddjvu_page_get_width(page);
        int ih = ddjvu_page_get_height(page);
        ddjvu_rect_t rect = {0, 0, (unsigned)iw, (unsigned)ih};
        ddjvu_format_t *fmt = ddjvu_format_create(DDJVU_FORMAT_GREY8, 0, 0);
        ddjvu_format_set_row_order(fmt, 1);
        unsigned char *image = (unsigned char *)malloc((size_t)iw * ih);
        t0 = now_ms();
        ddjvu_page_render(page, DDJVU_RENDER_COLOR, &rect, &rect, fmt, iw,
                          (char *)image);
        t1 = now_ms();
        if (t1 - t0 < best_render) best_render = t1 - t0;
        free(image);
        ddjvu_format_release(fmt);
        ddjvu_page_release(page);
        ddjvu_document_release(doc);
        printf("rep %d: create+decode %.2f render %.2f (%dx%d)\n",
               r, best_create, best_render, iw, ih);
    }
    printf("best: create+decode %.2f render %.2f ms\n", best_create, best_render);
    ddjvu_context_release(ctx);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: jb2prof dict.djbz page.sjbz [reps]\n"
                        "       jb2prof -ddjvu file.djvu <page0> [reps]\n");
        return 2;
    }
    if (!strcmp(argv[1], "-ddjvu"))
        return run_ddjvu(argv[2], atoi(argv[3]), argc > 4 ? atoi(argv[4]) : 5);
    int reps = argc > 3 ? atoi(argv[3]) : 5;
    GP<ByteStream> dictbs = ByteStream::create(GURL::Filename::UTF8(argv[1]), "rb");
    GP<ByteStream> pagebs = ByteStream::create(GURL::Filename::UTF8(argv[2]), "rb");
    GP<ByteStream> dictmem = ByteStream::create();
    GP<ByteStream> pagemem = ByteStream::create();
    dictmem->copy(*dictbs);
    pagemem->copy(*pagebs);

    double best_dict = 1e9, best_page = 1e9, best_render = 1e9;
    for (int r = 0; r < reps; r++) {
        dictmem->seek(0);
        double t0 = now_ms();
        GP<JB2Dict> dict = JB2Dict::create();
        dict->decode(dictmem);
        double t1 = now_ms();
        if (t1 - t0 < best_dict) best_dict = t1 - t0;

        pagemem->seek(0);
        t0 = now_ms();
        GP<JB2Image> img = JB2Image::create();
        img->decode(pagemem, dict_cb, &dict);
        t1 = now_ms();
        if (t1 - t0 < best_page) best_page = t1 - t0;

        t0 = now_ms();
        GP<GBitmap> bm = img->get_bitmap();
        t1 = now_ms();
        if (t1 - t0 < best_render) best_render = t1 - t0;
        printf("rep %d: dict %.2f page %.2f render %.2f (bm %dx%d, dict shapes %d, "
               "page shapes %d, blits %d)\n",
               r, best_dict, best_page, best_render,
               bm ? (int)bm->columns() : 0, bm ? (int)bm->rows() : 0,
               dict->get_shape_count(), img->get_shape_count(),
               img->get_blit_count());
    }
    printf("best: dict %.2f page %.2f render %.2f ms\n",
           best_dict, best_page, best_render);
    return 0;
}
