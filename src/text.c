/* text.c -- hidden text extraction. Ported from DjvuNet DataChunks/Text.
 * TXTz = BZZ-compressed, TXTa = raw; payload is u24-BE length + UTF-8 text
 * (followed by a text-zone tree which we skip). */
#include "djvu_internal.h"
#include <string.h>

char *djvu_page_text(djvu_doc *doc, int page_no)
{
    djvu_ctx *ctx;
    uint32_t form_off, sz;
    const uint8_t *chunk;
    const uint8_t *payload;
    uint8_t *decoded = NULL;
    size_t plen;
    uint32_t tlen;
    char *out;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;

    chunk = djvu_form_find_chunk(doc, form_off, "TXTz", &sz, NULL);
    if (chunk) {
        decoded = djvu_bzz_decode_all(ctx, chunk, sz, &plen);
        if (!decoded) return NULL;
        payload = decoded;
    } else {
        chunk = djvu_form_find_chunk(doc, form_off, "TXTa", &sz, NULL);
        if (!chunk) return NULL;
        payload = chunk;
        plen = sz;
    }

    if (plen < 3) { djvu_free(ctx, decoded); return NULL; }
    tlen = djvu_rd_u24be(payload);
    if ((size_t)tlen + 3 > plen) tlen = (uint32_t)(plen - 3);

    out = (char *)djvu_alloc(ctx, tlen + 1);
    if (out) {
        memcpy(out, payload + 3, tlen);
        out[tlen] = 0;
    }
    djvu_free(ctx, decoded);
    return out;
}

void djvu_text_destroy(djvu_ctx *ctx, char *text)
{
    djvu_free(ctx, text);
}
