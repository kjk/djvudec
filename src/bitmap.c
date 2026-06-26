/* bitmap.c -- GBitmap port (DjvuNet Graphics/Bitmap.cs + DjVuLibre RLE). */
#include "djvu_internal.h"
#include <string.h>

enum {
    DJVU_BM_RUN_OVERFLOW = 0xc0,
    DJVU_BM_MAX_RUN = 0x3fff
};

static int djvu_bm_alloc_guard(djvu_ctx *ctx, djvu_bitmap *bm)
{
    size_t gs = (size_t)bm->bytes_per_row + (size_t)bm->border;
    djvu_free(ctx, bm->guard);
    bm->guard = NULL;
    if (gs == 0) return 0;
    bm->guard = (uint8_t *)djvu_alloc(ctx, gs);
    if (!bm->guard) return -1;
    memset(bm->guard, 0, gs);
    return 0;
}

static int bm_read_run(const uint8_t **data)
{
    int z = *(*data)++;
    if (z >= DJVU_BM_RUN_OVERFLOW)
        z = ((z & ~DJVU_BM_RUN_OVERFLOW) << 8) | (int)(*(*data)++);
    return z;
}

static void bm_append_long_run(uint8_t **data, int count)
{
    while (count > DJVU_BM_MAX_RUN) {
        (*data)[0] = (*data)[1] = 0xff;
        (*data)[2] = 0;
        *data += 3;
        count -= DJVU_BM_MAX_RUN;
    }
    if (count < DJVU_BM_RUN_OVERFLOW) {
        (*data)[0] = (uint8_t)count;
        *data += 1;
    } else {
        (*data)[0] = (uint8_t)((count >> 8) + DJVU_BM_RUN_OVERFLOW);
        (*data)[1] = (uint8_t)(count & 0xff);
        *data += 2;
    }
}

static void bm_append_run(uint8_t **data, int count)
{
    if (count < DJVU_BM_RUN_OVERFLOW) {
        (*data)[0] = (uint8_t)count;
        *data += 1;
    } else if (count <= DJVU_BM_MAX_RUN) {
        (*data)[0] = (uint8_t)((count >> 8) + DJVU_BM_RUN_OVERFLOW);
        (*data)[1] = (uint8_t)(count & 0xff);
        *data += 2;
    } else {
        bm_append_long_run(data, count);
    }
}

static void bm_append_line(uint8_t **data, const uint8_t *row, int rowlen)
{
    const uint8_t *rowend = row + rowlen;
    int p = 1; /* matches GBitmap::append_line with invert=false */

    while (row < rowend) {
        int count = 0;
        p = !p;
        if (p) {
            if (*row)
                for (++count, ++row; row < rowend && *row; ++count, ++row)
                    ;
        } else if (!*row) {
            for (++count, ++row; row < rowend && !*row; ++count, ++row)
                ;
        }
        bm_append_run(data, count);
    }
}

static size_t bm_encode_rle(djvu_ctx *ctx, const djvu_bitmap *bm,
                            uint8_t **out_rle)
{
    int pos = 0, maxpos, n;
    uint8_t *runs, *runs_pos;
    const uint8_t *row;

    if (!bm->data || bm->width <= 0 || bm->height <= 0)
        return 0;

    maxpos = 1024 + bm->width + bm->width;
    runs = (uint8_t *)djvu_alloc(ctx, (size_t)maxpos);
    if (!runs) return 0;

    n = bm->height - 1;
    row = bm->data + djvu_bm_rowoffset(bm, n);
    while (n >= 0) {
        if (maxpos < pos + bm->width + bm->width + 2) {
            uint8_t *nr;
            maxpos += 1024 + bm->width + bm->width;
            nr = (uint8_t *)djvu_alloc(ctx, (size_t)maxpos);
            if (!nr) { djvu_free(ctx, runs); return 0; }
            memcpy(nr, runs, (size_t)pos);
            djvu_free(ctx, runs);
            runs = nr;
        }
        runs_pos = runs + pos;
        {
            const uint8_t *start = runs_pos;
            bm_append_line(&runs_pos, row, bm->width);
            pos += (int)(runs_pos - start);
        }
        row -= bm->bytes_per_row;
        n--;
    }

    *out_rle = runs;
    return (size_t)pos;
}

int djvu_bm_init(djvu_ctx *ctx, djvu_bitmap *bm, int height, int width, int border)
{
    long max;
    djvu_free(ctx, bm->data);
    djvu_free(ctx, bm->guard);
    djvu_free(ctx, bm->rle);
    bm->data = NULL;
    bm->guard = NULL;
    bm->rle = NULL;
    bm->rle_len = 0;
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
    return djvu_bm_alloc_guard(ctx, bm);
}

void djvu_bm_free(djvu_ctx *ctx, djvu_bitmap *bm)
{
    if (bm) {
        djvu_free(ctx, bm->data);
        djvu_free(ctx, bm->guard);
        djvu_free(ctx, bm->rle);
        bm->data = NULL;
        bm->guard = NULL;
        bm->rle = NULL;
        bm->rle_len = 0;
    }
}

void djvu_bm_uncompress(djvu_ctx *ctx, djvu_bitmap *bm)
{
    const uint8_t *runs;
    int c, n, p, x;

    if (!bm || bm->data || !bm->rle || bm->width <= 0 || bm->height <= 0)
        return;

    bm->bytes_per_row = bm->width + bm->border;
    bm->max_offset = bm->height * bm->bytes_per_row + bm->border;
    bm->data = (uint8_t *)djvu_alloc(ctx, (size_t)bm->max_offset);
    if (!bm->data) return;
    memset(bm->data, 0, (size_t)bm->max_offset);
    if (djvu_bm_alloc_guard(ctx, bm) != 0) {
        djvu_free(ctx, bm->data);
        bm->data = NULL;
        return;
    }

    runs = bm->rle;
    n = bm->height - 1;
    c = 0;
    p = 0;
    while (n >= 0) {
        x = bm_read_run(&runs);
        if (c + x > bm->width)
            break;
        while (x-- > 0)
            bm->data[djvu_bm_rowoffset(bm, n) + c++] = (uint8_t)p;
        p = 1 - p;
        if (c >= bm->width) {
            c = 0;
            p = 0;
            n--;
        }
    }

    djvu_free(ctx, bm->rle);
    bm->rle = NULL;
    bm->rle_len = 0;
}

void djvu_bm_ensure_bytes(djvu_ctx *ctx, djvu_bitmap *bm)
{
    if (bm && !bm->data && bm->rle)
        djvu_bm_uncompress(ctx, bm);
}

void djvu_bm_compress(djvu_ctx *ctx, djvu_bitmap *bm)
{
    uint8_t *nrle;
    size_t len;

    if (!bm || !bm->data || bm->width <= 0 || bm->height <= 0)
        return;

    len = bm_encode_rle(ctx, bm, &nrle);
    if (!len) return;

    djvu_free(ctx, bm->data);
    bm->data = NULL;
    djvu_free(ctx, bm->rle);
    bm->rle = nrle;
    bm->rle_len = len;
}

int djvu_bm_set_min_border(djvu_ctx *ctx, djvu_bitmap *bm, int value)
{
    int new_bpr, r;
    long new_max;
    uint8_t *nd;

    if (!bm->data && bm->rle)
        djvu_bm_uncompress(ctx, bm);
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
    return djvu_bm_alloc_guard(ctx, bm);
}

static void bm_blit_bytes(djvu_bitmap *dst, const djvu_bitmap *src,
                          int x0, int y0, int x1, int y1, int w, int h, int maxval)
{
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

static void bm_blit_rle(djvu_bitmap *dst, const djvu_bitmap *src,
                        int dx, int dy, int maxval)
{
    const uint8_t *runs = src->rle;
    const uint8_t *runs_end = src->rle + src->rle_len;
    int sr = src->height - 1;
    int sc = 0, p = 0;

    if (!dst->data) return;

    while (runs < runs_end && sr >= 0) {
        int z = bm_read_run(&runs);
        int nc;

        if (sc + z > src->width) return;
        nc = sc + z;

        if (p) {
            int dest_row = dy + sr;
            if (dest_row >= 0 && dest_row < dst->height) {
                uint8_t *drow = dst->data + djvu_bm_rowoffset(dst, dest_row);
                int col = sc;
                if (dx < 0) {
                    if (nc <= -dx) goto next_run;
                    col = sc + (-dx);
                }
                while (col < nc) {
                    int px = dx + col;
                    if (px >= dst->width) break;
                    if (px >= 0) {
                        int g = drow[px] + 1;
                        drow[px] = (uint8_t)(g <= maxval ? g : maxval);
                    }
                    col++;
                }
            }
        }
    next_run:
        sc = nc;
        p = 1 - p;
        if (sc >= src->width) {
            sc = 0;
            p = 0;
            sr--;
        }
    }
}

/* InsertMap(doBlit=true): dst[off] = min(dst[off]+src[off], maxval), bottom-up. */
void djvu_bm_blit(djvu_bitmap *dst, const djvu_bitmap *src, int dx, int dy, int maxval)
{
    int x0, y0, x1, y1, w0, w1, w, h0, h1, h;

    if (!dst || !src || !dst->data) return;
    if ((dx >= dst->width) || (dy >= dst->height) ||
        (dx + src->width < 0) || (dy + src->height < 0))
        return;

    if (src->data) {
        x0 = dx > 0 ? dx : 0;
        y0 = dy > 0 ? dy : 0;
        x1 = dx < 0 ? -dx : 0;
        y1 = dy < 0 ? -dy : 0;
        w0 = dst->width - x0;
        w1 = src->width - x1;
        w = w0 < w1 ? w0 : w1;
        h0 = dst->height - y0;
        h1 = src->height - y1;
        h = h0 < h1 ? h0 : h1;
        if (w <= 0 || h <= 0) return;
        bm_blit_bytes(dst, src, x0, y0, x1, y1, w, h, maxval);
        return;
    }

    if (src->rle)
        bm_blit_rle(dst, src, dx, dy, maxval);
}

static void bm_visit_ink_bytes(const djvu_bitmap *src, int left, int bottom,
                               void (*fn)(void *, int, int), void *user)
{
    int rr, cc, sw = src->width, sh = src->height;

    for (rr = 0; rr < sh; rr++) {
        int srow = djvu_bm_rowoffset(src, rr);
        int py = bottom + rr;
        for (cc = 0; cc < sw; cc++) {
            if (src->data[srow + cc])
                fn(user, left + cc, py);
        }
    }
}

static void bm_visit_ink_rle(const djvu_bitmap *src, int left, int bottom,
                             void (*fn)(void *, int, int), void *user)
{
    const uint8_t *runs = src->rle;
    const uint8_t *runs_end = src->rle + src->rle_len;
    int sr = src->height - 1;
    int sc = 0, p = 0;

    while (runs < runs_end && sr >= 0) {
        int z = bm_read_run(&runs);
        int nc;

        if (sc + z > src->width) return;
        nc = sc + z;

        if (p) {
            int py = bottom + sr;
            int col = sc;
            while (col < nc)
                fn(user, left + col++, py);
        }
        sc = nc;
        p = 1 - p;
        if (sc >= src->width) {
            sc = 0;
            p = 0;
            sr--;
        }
    }
}

void djvu_bm_visit_ink(const djvu_bitmap *src, int left, int bottom,
                       void (*fn)(void *user, int px, int py), void *user)
{
    if (!src || !fn) return;
    if (src->data)
        bm_visit_ink_bytes(src, left, bottom, fn, user);
    else if (src->rle)
        bm_visit_ink_rle(src, left, bottom, fn, user);
}

static void bm_bbox_rle(const djvu_bitmap *bm, int *xmin, int *ymin, int *xmax, int *ymax)
{
    const uint8_t *runs = bm->rle;
    int w = bm->width, h = bm->height;
    int xa = w, ya = h, xb = 0, yb = 0, area = 0;
    int r = h;

    while (--r >= 0) {
        int p = 0, c = 0, n = 0;
        while (c < w) {
            int x = bm_read_run(&runs);
            if (x) {
                if (p) {
                    if (c < xa) xa = c;
                    c += x;
                    if (c - 1 > xb) xb = c - 1;
                    n += x;
                } else {
                    c += x;
                }
            }
            p = 1 - p;
        }
        area += n;
        if (n) {
            ya = r;
            if (r > yb) yb = r;
        }
    }
    if (area == 0) {
        *xmin = 1; *ymin = 1; *xmax = 0; *ymax = 0;
        return;
    }
    *xmin = xa; *ymin = ya; *xmax = xb; *ymax = yb;
}

/* matches Bitmap.ComputeBoundingBox using GetBooleanAt (data==0 background) */
void djvu_bm_bbox(const djvu_bitmap *bm, int *xmin, int *ymin, int *xmax, int *ymax)
{
    int w = bm->width, h = bm->height, s = bm->bytes_per_row;
    int xa, xb, ya, yb;

    if (!bm->data && bm->rle) {
        bm_bbox_rle(bm, xmin, ymin, xmax, ymax);
        return;
    }

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