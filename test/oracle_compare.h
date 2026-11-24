/* oracle_compare.h -- compare our render with DjVuLibre's (ddjvuapi).
 *
 * Shared by the oracle verifier (djvu_test -verify-render) and the
 * differential fuzz target (fuzz_diff.c). Implements the one accepted
 * difference, the palette-overlap rule (AGENTS.md): on FGbz palette pages
 * DjVuLibre's compositor adds overlapping colour-group blits (clipped at 255)
 * where we paint one colour. Header-only; needs djvu_internal.h (page FORM /
 * FGbz access) and <math.h>. */
#ifndef ORACLE_COMPARE_H
#define ORACLE_COMPARE_H

#include "harness_util.h"
#include "djvu_internal.h"
#include <math.h>

/* A ddjvuapi render (bench_ddjvu.cpp / fuzz_ddjvu.cpp): top-down rows of
   rowsize bytes, bps 1 (GREY8) or 3 (BGR24). */
typedef struct bench_render {
    int width, height;
    int bps;
    int rowsize;
    uint8_t *data;
} bench_render;

/* Result of comparing our render with the oracle's. */
enum { IMG_DIFF = 0, IMG_SAME = 1, IMG_OVERLAP = 2 };

/* Number of differing pixels between two same-size RGB images. */
HU_UNUSED static size_t image_rgb_diff_pixels(const djvu_image *mine, const bench_render *ref)
{
    size_t diffs = 0;
    int y, x;
    for (y = 0; y < mine->height; y++) {
        const uint8_t *a = mine->data + (size_t)y * (size_t)mine->stride;
        const uint8_t *b = ref->data + (size_t)y * (size_t)ref->rowsize;
        for (x = 0; x < mine->width; x++) {
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) diffs++;
            a += 3;
            b += 3;
        }
    }
    return diffs;
}

/* Palette (FGbz) colours of a page as the compositor paints them, passed
   through the same gamma correction (target 2.2 / INFO gamma) as compose.c.
   Kept in the chunk's B,G,R byte order: -verify-render compares BGR images
   (djvu_ctx_set_bgr + ddjvu BGR24), and the overlap rule is per channel.
   Returns the colour count (0 if the page has no FGbz palette); *out is
   malloc'd, caller frees. */
HU_UNUSED static int page_palette_rgb(djvu_doc *doc, int page0, uint8_t **out)
{
    uint32_t form_off = doc->pages[page0].form_off, sz, isz;
    const uint8_t *fg = djvu_form_find_chunk(doc, form_off, "FGbz", &sz, NULL);
    const uint8_t *info = djvu_form_find_chunk(doc, form_off, "INFO", &isz, NULL);
    unsigned char lut[256];
    double corr = 1.0;
    int n, i;
    uint8_t *pal;

    *out = NULL;
    if (!fg || sz < 3) return 0;
    n = (fg[1] << 8) | fg[2];
    if (n <= 0 || 3 + (size_t)n * 3 > sz) return 0;
    if (info && isz >= 9) {
        /* gamma as compose.c page_gamma / DjVuInfo::decode: clamp [0.3, 5] */
        double g = (double)info[8] / 10.0;
        if (g < 0.3) g = 0.3;
        if (g > 5.0) g = 5.0;
        corr = 2.2 / g;
    }
    if (corr < 0.1) corr = 0.1; else if (corr > 10.0) corr = 10.0;
    for (i = 0; i < 256; i++) {
        int v = i;
        if (!(corr > 0.999 && corr < 1.001))
            v = (int)floor(255.0 * pow((double)i / 255.0, 1.0 / corr) + 0.5);
        lut[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    lut[0] = 0; lut[255] = 255;
    pal = (uint8_t *)malloc((size_t)n * 3);
    if (!pal) return 0;
    for (i = 0; i < n; i++) {
        pal[i * 3 + 0] = lut[fg[3 + i * 3 + 0]];   /* B */
        pal[i * 3 + 1] = lut[fg[3 + i * 3 + 1]];   /* G */
        pal[i * 3 + 2] = lut[fg[3 + i * 3 + 2]];   /* R */
    }
    *out = pal;
    return n;
}

HU_UNUSED static int pal_has(const uint8_t *pal, int n, const uint8_t *px)
{
    int i;
    for (i = 0; i < n; i++)
        if (pal[i * 3] == px[0] && pal[i * 3 + 1] == px[1] && pal[i * 3 + 2] == px[2])
            return 1;
    return 0;
}

/* clip(a + c1 [+ c2]) == r on every channel? (c2 may be NULL) */
HU_UNUSED static int clip_sum_eq(const uint8_t *a, const uint8_t *c1, const uint8_t *c2,
                       const uint8_t *r)
{
    int k;
    for (k = 0; k < 3; k++) {
        int s = a[k] + c1[k] + (c2 ? c2[k] : 0);
        if ((s > 255 ? 255 : s) != r[k]) return 0;
    }
    return 1;
}

/* Palettes up to this size get the three-group (pairwise) check; real FGbz
   palettes have tens to a few hundred colours. */
#define OVERLAP_PAIR_MAX_PAL 1024

/* DjVuLibre's palette compositor (DjVuImage::stencil, two-layer model) zeroes
   every mask pixel, then blends each colour group in by ADDING its colour,
   clipped at 255 (GPixmap::blit). Where blits of two or three colour groups
   overlap, a pixel gets the clipped sum of their colours; we paint one blit's
   colour. So on a palette page a differing pixel is explained when ours is a
   palette colour A and the oracle's is clip(A + C1) or clip(A + C1 + C2) for
   palette colours C1, C2. */
HU_UNUSED static int pixel_is_palette_overlap(const uint8_t *a, const uint8_t *r,
                                    const uint8_t *pal, int n)
{
    int i, j;
    if (!pal_has(pal, n, a)) return 0;
    for (i = 0; i < n; i++)
        if (clip_sum_eq(a, pal + i * 3, NULL, r)) return 1;
    if (n > OVERLAP_PAIR_MAX_PAL) return 0;
    for (i = 0; i < n; i++)
        for (j = i; j < n; j++)
            if (clip_sum_eq(a, pal + i * 3, pal + j * 3, r)) return 1;
    return 0;
}

/* IMG_SAME: byte-identical. IMG_OVERLAP: a color palette (FGbz) page where
   every differing pixel is explained by DjVuLibre's additive blend at
   overlapping blits of two or three colour groups (pixel_is_palette_overlap; *ndiff
   receives the count). Our mask, background and colours are unaffected;
   the caller reports these pages distinctly. IMG_DIFF: anything else,
   including any pixel the rule does not explain. */
HU_UNUSED static int image_compare(djvu_doc *doc, int page0, const djvu_image *mine,
                         const bench_render *ref, size_t *ndiff)
{
    int x, y, same = 1, npal, verdict = IMG_OVERLAP;
    size_t row, diffs = 0, total;
    uint8_t *pal = NULL;

    *ndiff = 0;
    if (!mine || !ref || !ref->data) return IMG_DIFF;
    if (mine->width != ref->width || mine->height != ref->height) return IMG_DIFF;
    if ((int)mine->format != ref->bps) return IMG_DIFF;
    row = (size_t)mine->width * (size_t)mine->format;
    for (y = 0; y < mine->height && same; y++)
        if (memcmp(mine->data + (size_t)y * (size_t)mine->stride,
                   ref->data + (size_t)y * (size_t)ref->rowsize, row) != 0)
            same = 0;
    if (same) return IMG_SAME;
    if (mine->format != DJVU_FORMAT_RGB24) return IMG_DIFF;

    npal = page_palette_rgb(doc, page0, &pal);
    if (npal == 0) return IMG_DIFF;
    total = (size_t)mine->width * (size_t)mine->height;
    for (y = 0; y < mine->height && verdict == IMG_OVERLAP; y++) {
        const uint8_t *a = mine->data + (size_t)y * (size_t)mine->stride;
        const uint8_t *r = ref->data + (size_t)y * (size_t)ref->rowsize;
        for (x = 0; x < mine->width; x++, a += 3, r += 3) {
            if (a[0] == r[0] && a[1] == r[1] && a[2] == r[2]) continue;
            diffs++;
            /* overlaps are rare specks; a page-wide difference is a real
               mismatch, so don't spend time explaining it pixel by pixel */
            if (diffs * 100 > total || !pixel_is_palette_overlap(a, r, pal, npal)) {
                verdict = IMG_DIFF;
                break;
            }
        }
    }
    free(pal);
    if (verdict == IMG_DIFF) {
        *ndiff = image_rgb_diff_pixels(mine, ref);
        return IMG_DIFF;
    }
    *ndiff = diffs;
    return IMG_OVERLAP;
}

#endif /* ORACLE_COMPARE_H */
