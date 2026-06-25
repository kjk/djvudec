/* render.c -- page rendering. Milestone 3: JB2 bitonal composite.
 * (color IW44 = milestone 5). */
#include "djvu_internal.h"
#include "djvu_jb2.h"
#include "djvu_bitmap.h"
#include <string.h>

/* trim trailing whitespace/control from an INCL id copied into buf */
static void trim_id(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == 0x1a))
        s[--n] = 0;
}

/* Decode the JB2 shape dictionary for a page: either embedded directly in the
   page form as a Djbz chunk, or referenced by an INCL chunk pointing at a
   shared DJVI component that contains the Djbz. */
static jb2_image *load_page_dict(djvu_doc *doc, uint32_t form_off)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t start = 0, sz;
    const uint8_t *incl;
    const uint8_t *djbz_inpage;

    /* in-page Djbz takes precedence */
    djbz_inpage = djvu_form_find_chunk(doc, form_off, "Djbz", &sz, NULL);
    if (djbz_inpage)
        return djvu_jb2_decode_dict(ctx, djbz_inpage, sz);

    while ((incl = djvu_form_find_chunk(doc, form_off, "INCL", &sz, &start)) != NULL) {
        char id[64];
        uint32_t coff, dsz;
        const uint8_t *djbz;
        size_t n = sz < sizeof(id) - 1 ? sz : sizeof(id) - 1;
        memcpy(id, incl, n);
        id[n] = 0;
        trim_id(id);
        coff = djvu_doc_component_offset(doc, id);
        if (!coff) continue;
        djbz = djvu_form_find_chunk(doc, coff, "Djbz", &dsz, NULL);
        if (djbz)
            return djvu_jb2_decode_dict(ctx, djbz, dsz);
    }
    return NULL;
}

djvu_image *djvu_page_render(djvu_doc *doc, int page_no, int subsample)
{
    djvu_ctx *ctx;
    djvu_page_int *pg;
    uint32_t form_off, sz;
    const uint8_t *sjbz;
    jb2_image *dict = NULL, *img = NULL;
    djvu_bitmap page;
    djvu_image *out = NULL;
    int i, y, x;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    if (subsample < 1) subsample = 1;
    ctx = doc->ctx;
    pg = &doc->pages[page_no];
    form_off = pg->form_off;

    sjbz = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL);
    if (!sjbz) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR,
                    "page %d: no Sjbz (color pages not yet supported)", page_no);
        return NULL;
    }

    dict = load_page_dict(doc, form_off);
    img = djvu_jb2_decode(ctx, sjbz, sz, dict);
    if (!img) goto done;

    /* compose mask into a full-res bottom-up bitmap (Grays=2 -> clamp 1) */
    memset(&page, 0, sizeof(page));
    if (djvu_bm_init(ctx, &page, img->height, img->width, 0) != 0) goto done;
    for (i = 0; i < img->nblits; i++) {
        jb2_blit *b = &img->blits[i];
        jb2_shape *s = djvu_jb2_get_shape(img, b->shapeno);
        if (s && s->bm.data)
            djvu_bm_blit(&page, &s->bm, b->left, b->bottom, 1);
    }

    /* emit gray8, top-down, ink(1)->0 white(0)->255, with box subsampling */
    {
        int sw = (img->width + subsample - 1) / subsample;
        int sh = (img->height + subsample - 1) / subsample;
        out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
        if (!out) { djvu_bm_free(ctx, &page); goto done; }
        out->width = sw; out->height = sh;
        out->format = DJVU_FORMAT_GRAY8;
        out->stride = sw;
        out->data = (uint8_t *)djvu_alloc(ctx, (size_t)sw * sh);
        if (!out->data) { djvu_free(ctx, out); out = NULL; djvu_bm_free(ctx, &page); goto done; }

        if (subsample == 1) {
            for (y = 0; y < img->height; y++) {
                /* page row (img->height-1-y) is top row y (flip) */
                int srow = djvu_bm_rowoffset(&page, img->height - 1 - y);
                uint8_t *dst = out->data + (size_t)y * sw;
                for (x = 0; x < img->width; x++)
                    dst[x] = page.data[srow + x] ? 0 : 255;
            }
        } else {
            for (y = 0; y < sh; y++) {
                uint8_t *dst = out->data + (size_t)y * sw;
                for (x = 0; x < sw; x++) {
                    int cnt = 0, tot = 0, yy, xx;
                    for (yy = 0; yy < subsample; yy++) {
                        int py = y * subsample + yy;
                        int srow;
                        if (py >= img->height) break;
                        srow = djvu_bm_rowoffset(&page, img->height - 1 - py);
                        for (xx = 0; xx < subsample; xx++) {
                            int px = x * subsample + xx;
                            if (px >= img->width) continue;
                            tot++;
                            if (page.data[srow + px]) cnt++;
                        }
                    }
                    dst[x] = tot ? (uint8_t)(255 - (cnt * 255 / tot)) : 255;
                }
            }
        }
    }
    djvu_bm_free(ctx, &page);

done:
    djvu_jb2_free(ctx, img);
    djvu_jb2_free(ctx, dict);
    return out;
}

void djvu_image_destroy(djvu_ctx *ctx, djvu_image *img)
{
    if (img) {
        djvu_free(ctx, img->data);
        djvu_free(ctx, img);
    }
}
