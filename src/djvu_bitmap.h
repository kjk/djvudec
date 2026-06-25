/* djvu_bitmap.h -- 8bpp gray bitmap with a context border (GBitmap port).
 * Used for JB2 shape/mask bitmaps. 0 = background(white), >0 = ink. */
#ifndef DJVU_BITMAP_H
#define DJVU_BITMAP_H

#include "djvu_internal.h"

typedef struct {
    int width, height;
    int border;
    int bytes_per_row;   /* width + border */
    int max_offset;      /* height*bytes_per_row + border = length of data */
    uint8_t *data;
} djvu_bitmap;

/* (re)initialize bm to height x width with the given context border (zeroed). */
int  djvu_bm_init(djvu_ctx *ctx, djvu_bitmap *bm, int height, int width, int border);
void djvu_bm_free(djvu_ctx *ctx, djvu_bitmap *bm);

static inline int djvu_bm_rowoffset(const djvu_bitmap *bm, int row) {
    return row * bm->bytes_per_row + bm->border;
}
static inline int djvu_bm_get(const djvu_bitmap *bm, int offset) {
    return (offset < bm->border || offset >= bm->max_offset) ? 0 : bm->data[offset];
}
static inline void djvu_bm_set(djvu_bitmap *bm, int offset, int v) {
    bm->data[offset] = (uint8_t)v;
}

/* grow the border to at least `value`, preserving pixels. */
int djvu_bm_set_min_border(djvu_ctx *ctx, djvu_bitmap *bm, int value);

/* tight bounding box of non-zero pixels; if empty, returns xmin>xmax. */
void djvu_bm_bbox(const djvu_bitmap *bm, int *xmin, int *ymin, int *xmax, int *ymax);

/* OR-blit src into dst at (dx,dy) (bottom-up coords), clamping to maxval. */
void djvu_bm_blit(djvu_bitmap *dst, const djvu_bitmap *src, int dx, int dy, int maxval);

#endif /* DJVU_BITMAP_H */
