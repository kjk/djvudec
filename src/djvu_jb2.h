/* djvu_jb2.h -- JB2 bitonal decoder (DjvuNet JB2 modules, decode only). */
#ifndef DJVU_JB2_H
#define DJVU_JB2_H

#include "djvu_internal.h"
#include "djvu_bitmap.h"

typedef struct {
    int parent;
    djvu_bitmap bm;
} jb2_shape;

typedef struct { int left, bottom, shapeno; } jb2_blit;

typedef struct jb2_image {
    djvu_ctx *ctx;
    /* dictionary part */
    jb2_shape *shapes;
    int nshapes, cap_shapes;
    int inherited_shapes;
    struct jb2_image *inherited_dict;
    /* image part */
    int width, height;
    jb2_blit *blits;
    int nblits, cap_blits;
} jb2_image;

/* Decode a JB2 stream (Sjbz mask or Djbz dictionary) from [data,len).
   `dict` is the inherited shape dictionary (from a Djbz), or NULL.
   Returns a new jb2_image (free with djvu_jb2_free), NULL on error. */
jb2_image *djvu_jb2_decode(djvu_ctx *ctx, const uint8_t *data, size_t len,
                           jb2_image *dict);
/* Decode a Djbz shared dictionary stream (zero image size). */
jb2_image *djvu_jb2_decode_dict(djvu_ctx *ctx, const uint8_t *data, size_t len);
void djvu_jb2_free(djvu_ctx *ctx, jb2_image *img);

/* Resolve a shape number through the inheritance chain. */
jb2_shape *djvu_jb2_get_shape(jb2_image *img, int shapeno);

#endif /* DJVU_JB2_H */
