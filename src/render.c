/* render.c -- page rendering. Milestone 2+ (JB2 bitonal), 5 (IW44 color).
 * Currently a stub. */
#include "djvu_internal.h"

djvu_image *djvu_page_render(djvu_doc *doc, int page_no, int subsample)
{
    (void)subsample;
    if (doc)
        djvu_errorf(doc->ctx, DJVU_SEVERITY_ERROR,
                    "djvu_page_render: not implemented yet (page %d)", page_no);
    return NULL;
}

void djvu_image_destroy(djvu_ctx *ctx, djvu_image *img)
{
    if (img) {
        djvu_free(ctx, img->data);
        djvu_free(ctx, img);
    }
}
