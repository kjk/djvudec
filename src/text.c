/* text.c -- hidden text extraction. Milestone 3/4 (BZZ + Txta/Txtz).
 * Currently a stub. */
#include "djvu_internal.h"

char *djvu_page_text(djvu_doc *doc, int page_no)
{
    (void)doc; (void)page_no;
    return NULL;
}

void djvu_text_destroy(djvu_ctx *ctx, char *text)
{
    djvu_free(ctx, text);
}
