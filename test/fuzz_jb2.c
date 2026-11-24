/* fuzz_jb2.c -- libFuzzer target: JB2 (Sjbz mask + Djbz dictionary + FGbz).
 *
 * Input: [u8 opts][u24 n][Djbz n bytes][u24 m][FGbz m bytes][Sjbz: rest].
 * The streams are decoded through the internal JB2 API first; when the mask
 * has sane dimensions the same chunks are wrapped in a synthesized single-page
 * FORM:DJVU (INFO + Djbz + Sjbz + FGbz) and rendered through the public API
 * (bitonal stamp, subsampled coverage, FGbz palette compose, rotation). */
#include "fuzz_util.h"

#define FZ_MAX_PAGE_PIXELS ((unsigned long long)16 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_in in = { data, size }, dict, fgbz, sjbz;
    unsigned opts = fz_u8(&in);
    djvu_ctx *ctx;
    jb2_image *dimg = NULL, *img;
    int w = 0, h = 0, i;

    dict = fz_take(&in);
    fgbz = fz_take(&in);
    sjbz = in;

    ctx = fz_ctx_new(opts & ~FZ_OPT_CACHE);
    if (!ctx) return 0;
    if (dict.n) dimg = djvu_jb2_decode_dict(ctx, dict.p, dict.n);
    img = djvu_jb2_decode(ctx, sjbz.p, sjbz.n, dimg);
    if (img) {
        w = img->width; h = img->height;
        for (i = 0; i < img->nblits; i++)
            (void)djvu_jb2_get_shape(img, img->blits[i].shapeno);
        (void)djvu_jb2_mem_size(img);
        djvu_jb2_free(ctx, img);
    }
    if (dimg) djvu_jb2_free(ctx, dimg);
    fz_ctx_free(ctx);

    if (w > 0 && h > 0 && w <= 0xffff && h <= 0xffff &&
        (unsigned long long)w * (unsigned long long)h <= FZ_MAX_PAGE_PIXELS) {
        static const int rots[4] = { 1, 6, 2, 5 };
        fz_buf b = { 0 };
        size_t at = fz_page_begin(&b, w, h, rots[(opts >> 4) & 3]);
        if (dict.n) fz_chunk(&b, "Djbz", dict.p, dict.n);
        fz_chunk(&b, "Sjbz", sjbz.p, sjbz.n);
        if (fgbz.n) fz_chunk(&b, "FGbz", fgbz.p, fgbz.n);
        fz_page_end(&b, at);
        ctx = fz_ctx_new(opts);
        if (ctx) {
            fz_drive_buf(ctx, &b, 2 + ((opts >> 6) & 3), (opts >> 3) & 1,
                         (unsigned long long)2 << 20);
            fz_ctx_free(ctx);
        }
        free(b.d);
    }
    return 0;
}
