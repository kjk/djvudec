/* fuzz_iw44.c -- libFuzzer target: IW44 wavelet chunks (BG44/FG44/PM44/BM44).
 *
 * Input: [u8 opts] then a sequence of [u24 n][chunk n bytes] (progressive
 * refinement chunks of one layer, in order). Decoded through the internal
 * IW44 API and rendered; when the image is small enough the chunks are also
 * wrapped in a synthesized page and rendered through the public API:
 * opts bit 0 picks a FORM:DJVU background (BG44 + INFO) or a standalone
 * FORM:PM44 photo, which covers compose / scaler / subsampling. */
#include "fuzz_util.h"

#define FZ_MAX_CHUNKS 64
/* Decode any size the budget allows; render only small images. Per-input
   cost matters: libFuzzer replays the whole corpus at every start, and a
   full-page background at full res costs ~100 ms under ASan. */
#define FZ_MAX_IW_PIXELS ((unsigned long long)1 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_in in = { data, size }, chunks[FZ_MAX_CHUNKS];
    unsigned opts = fz_u8(&in);
    int nchunks = 0, i, w = 0, h = 0;
    djvu_ctx *ctx;
    iw_pixmap *pm;

    while (in.n && nchunks < FZ_MAX_CHUNKS) chunks[nchunks++] = fz_take(&in);

    ctx = fz_ctx_new(0);
    if (!ctx) return 0;
    pm = djvu_iw44_new(ctx);
    if (pm) {
        for (i = 0; i < nchunks; i++)
            if (djvu_iw44_decode_chunk(pm, chunks[i].p, chunks[i].n) != 0) break;
        w = djvu_iw44_width(pm);
        h = djvu_iw44_height(pm);
        (void)djvu_iw44_is_color(pm);
        (void)djvu_iw44_mem_size(pm);
        if (w > 0 && h > 0 &&
            (unsigned long long)w * (unsigned long long)h <= FZ_MAX_IW_PIXELS) {
            uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
            if (rgb) {
                djvu_iw44_render_rgb_raw(pm, rgb, 0);
                free(rgb);
            }
        }
        djvu_iw44_free(pm);
    }
    fz_ctx_free(ctx);

    if (nchunks && w > 0 && h > 0 && w <= 0xffff && h <= 0xffff &&
        (unsigned long long)w * (unsigned long long)h <= 2 * FZ_MAX_IW_PIXELS) {
        fz_buf b = { 0 };
        size_t at;
        if (opts & 1) {
            fz_put(&b, "AT&TFORM", 8);
            at = b.n;
            fz_put_u32be(&b, 0);
            fz_put(&b, "PM44", 4);
            for (i = 0; i < nchunks; i++) fz_chunk(&b, "PM44", chunks[i].p, chunks[i].n);
        } else {
            at = fz_page_begin(&b, w, h, 1);
            for (i = 0; i < nchunks; i++) fz_chunk(&b, "BG44", chunks[i].p, chunks[i].n);
        }
        fz_page_end(&b, at);
        ctx = fz_ctx_new(opts >> 1);
        if (ctx) {
            fz_drive_buf(ctx, &b, 2 + ((opts >> 5) & 3), (opts >> 7) & 1,
                         FZ_MAX_IW_PIXELS / 4);
            fz_ctx_free(ctx);
        }
        free(b.d);
    }
    return 0;
}
