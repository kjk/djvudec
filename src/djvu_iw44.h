/* djvu_iw44.h -- IW44 wavelet decoder (DjvuNet Wavelet/* port, decode only).
 * Decodes BG44/FG44/PM44/BM44 chunks into a Y[CbCr] pixmap. */
#ifndef DJVU_IW44_H
#define DJVU_IW44_H

#include "djvu_internal.h"

typedef struct iw_pixmap iw_pixmap;

iw_pixmap *djvu_iw44_new(djvu_ctx *ctx);
void djvu_iw44_free(iw_pixmap *pm);

/* Feed one IW44 chunk (e.g. one BG44). Chunks must arrive in serial order.
   Returns 0 on success, -1 on error. */
int djvu_iw44_decode_chunk(iw_pixmap *pm, const uint8_t *data, size_t len);

int djvu_iw44_width(iw_pixmap *pm);
int djvu_iw44_height(iw_pixmap *pm);
int djvu_iw44_is_color(iw_pixmap *pm);

/* Render full-resolution. `rgb` must hold width*height*3 bytes (R,G,B order).
   Gray images are expanded to gray RGB. Returns 0 on success. */
int djvu_iw44_render_rgb(iw_pixmap *pm, uint8_t *rgb);

/* Render full-resolution gray (1 byte/pixel, 0..255). Returns 0 on success. */
int djvu_iw44_render_gray(iw_pixmap *pm, uint8_t *gray);

/* debug: render plane 0=Y,1=Cb,2=Cr as gray (value+128). */
int djvu_iw44_render_plane(iw_pixmap *pm, int plane, uint8_t *gray);

#endif /* DJVU_IW44_H */
