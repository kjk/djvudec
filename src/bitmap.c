/* bitmap.c -- GBitmap port (DjvuNet Graphics/Bitmap.cs). */
#include "djvu_internal.h"
#include <string.h>

int djvu_bm_init(djvu_ctx *ctx, djvu_bitmap *bm, int height, int width, int border)
{
    long max;
    djvu_free(ctx, bm->data);
    bm->data = NULL;
    bm->width = width;
    bm->height = height;
    bm->border = border;
    bm->bytes_per_row = width + border;
    max = (long)height * bm->bytes_per_row + border;
    bm->max_offset = (int)max;
    if (max > 0) {
        bm->data = (uint8_t *)djvu_alloc(ctx, (size_t)max);
        if (!bm->data) return -1;
        memset(bm->data, 0, (size_t)max);
    }
    return 0;
}

void djvu_bm_free(djvu_ctx *ctx, djvu_bitmap *bm)
{
    if (bm) { djvu_free(ctx, bm->data); bm->data = NULL; }
}

int djvu_bm_set_min_border(djvu_ctx *ctx, djvu_bitmap *bm, int value)
{
    int new_bpr, r;
    long new_max;
    uint8_t *nd;
    if (bm->border >= value) return 0;
    new_bpr = bm->width + value;
    new_max = (long)bm->height * new_bpr + value;
    nd = (uint8_t *)djvu_alloc(ctx, (size_t)(new_max > 0 ? new_max : 1));
    if (!nd) return -1;
    memset(nd, 0, (size_t)(new_max > 0 ? new_max : 1));
    if (bm->data) {
        for (r = 0; r < bm->height; r++) {
            int src = r * bm->bytes_per_row + bm->border;
            int dst = r * new_bpr + value;
            memcpy(nd + dst, bm->data + src, (size_t)bm->width);
        }
    }
    djvu_free(ctx, bm->data);
    bm->data = nd;
    bm->border = value;
    bm->bytes_per_row = new_bpr;
    bm->max_offset = (int)new_max;
    return 0;
}

/* InsertMap(doBlit=true): dst[off] = min(dst[off]+src[off], maxval), bottom-up. */
void djvu_bm_blit(djvu_bitmap *dst, const djvu_bitmap *src, int dx, int dy, int maxval)
{
    int x0 = dx > 0 ? dx : 0;
    int y0 = dy > 0 ? dy : 0;
    int x1 = dx < 0 ? -dx : 0;
    int y1 = dy < 0 ? -dy : 0;
    int w0 = dst->width - x0, w1 = src->width - x1;
    int w = w0 < w1 ? w0 : w1;
    int h0 = dst->height - y0, h1 = src->height - y1;
    int h = h0 < h1 ? h0 : h1;
    if (w <= 0 || h <= 0) return;
    do {
        int off = djvu_bm_rowoffset(dst, y0++) + x0;
        int roff = djvu_bm_rowoffset(src, y1++) + x1;
        int i = w;
        do {
            int g = dst->data[off] + src->data[roff++];
            dst->data[off++] = (uint8_t)(g <= maxval ? g : maxval);
        } while (--i > 0);
    } while (--h > 0);
}

/* matches Bitmap.ComputeBoundingBox using GetBooleanAt (data==0 background) */
void djvu_bm_bbox(const djvu_bitmap *bm, int *xmin, int *ymin, int *xmax, int *ymax)
{
    int w = bm->width, h = bm->height, s = bm->bytes_per_row;
    int xa, xb, ya, yb;

    for (xb = w - 1; xb >= 0; xb--) {
        int p = djvu_bm_rowoffset(bm, 0) + xb;
        int pe = p + s * h;
        while (p < pe && bm->data[p] == 0) p += s;
        if (p < pe) break;
    }
    for (yb = h - 1; yb >= 0; yb--) {
        int p = djvu_bm_rowoffset(bm, yb);
        int pe = p + w;
        while (p < pe && bm->data[p] == 0) ++p;
        if (p < pe) break;
    }
    for (xa = 0; xa <= xb; xa++) {
        int p = djvu_bm_rowoffset(bm, 0) + xa;
        int pe = p + s * h;
        while (p < pe && bm->data[p] == 0) p += s;
        if (p < pe) break;
    }
    for (ya = 0; ya <= yb; ya++) {
        int p = djvu_bm_rowoffset(bm, ya);
        int pe = p + w;
        while (p < pe && bm->data[p] == 0) ++p;
        if (p < pe) break;
    }
    *xmin = xa; *ymin = ya; *xmax = xb; *ymax = yb;
}
