/* text.c -- hidden text extraction. Ported from DjvuNet DataChunks/Text.
 * TXTz = BZZ-compressed, TXTa = raw. Payload: u24-BE text length, UTF-8 text,
 * version byte, then a recursive zone tree (per DjvuNet TextChunk/TextZone). */
#include "djvu_internal.h"
#include <string.h>

/* Locate and (if needed) decompress the text payload for a page.
   On success returns the payload pointer in *payload and its length in *plen,
   and sets *owned to a buffer the caller must djvu_free (NULL for raw TXTa).
   Returns 0 on success, -1 if the page has no text. */
static int load_text_payload(djvu_doc *doc, int page_no, uint8_t **owned,
                             const uint8_t **payload, size_t *plen)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t form_off, sz;
    const uint8_t *chunk;

    *owned = NULL;
    form_off = doc->pages[page_no].form_off;

    chunk = djvu_form_find_chunk(doc, form_off, "TXTz", &sz, NULL);
    if (chunk) {
        size_t dlen;
        uint8_t *dec = djvu_bzz_decode_all(ctx, chunk, sz, &dlen);
        if (!dec) return -1;
        *owned = dec; *payload = dec; *plen = dlen;
        return 0;
    }
    chunk = djvu_form_find_chunk(doc, form_off, "TXTa", &sz, NULL);
    if (!chunk) return -1;
    *payload = chunk; *plen = sz;
    return 0;
}

char *djvu_page_text(djvu_doc *doc, int page_no)
{
    djvu_ctx *ctx;
    uint8_t *owned = NULL;
    const uint8_t *payload;
    size_t plen;
    uint32_t tlen;
    char *out;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    if (load_text_payload(doc, page_no, &owned, &payload, &plen) != 0) return NULL;

    if (plen < 3) { djvu_free(ctx, owned); return NULL; }
    tlen = djvu_rd_u24be(payload);
    if ((size_t)tlen + 3 > plen) tlen = (uint32_t)(plen - 3);

    out = (char *)djvu_alloc(ctx, tlen + 1);
    if (out) {
        memcpy(out, payload + 3, tlen);
        out[tlen] = 0;
    }
    djvu_free(ctx, owned);
    return out;
}

void djvu_text_destroy(djvu_ctx *ctx, char *text)
{
    djvu_free(ctx, text);
}

/* ---- structured text (zone tree) ---- */

typedef struct {
    djvu_buf_reader br;
    const char *text;     /* full UTF-8 text */
    size_t textlen;
} zparse;

/* Copy the covered text [off, off+len) as a fresh NUL-terminated string. */
static char *zone_text(zparse *z, int off, int len)
{
    char *s;
    int avail;
    if (off < 0) off = 0;
    if (off > (int)z->textlen) off = (int)z->textlen;
    avail = (int)z->textlen - off;
    if (len < 0) len = 0;
    if (len > avail) len = avail;
    s = (char *)djvu_alloc(z->br.ctx, (size_t)len + 1);
    if (!s) return NULL;
    memcpy(s, z->text + off, (size_t)len);
    s[len] = 0;
    return s;
}

static void free_zone_kids(djvu_ctx *ctx, djvu_text_zone *z);

/* Parse one zone into *out (coords kept bottom-up here), resolving offsets
   relative to its parent / previous sibling exactly as DjvuNet TextZone does.
   Returns 0 on success. */
static int parse_zone(zparse *z, djvu_text_zone *out,
                      const djvu_text_zone *parent, const djvu_text_zone *sib,
                      int parent_toff, int sib_toff, int sib_tlen,
                      int *out_toff, int *out_tlen)
{
    int type, x, y, w, h, toff, tlen, nkids, i;
    djvu_text_zone *prev = NULL;
    int prev_toff = 0, prev_tlen = 0;

    type = djvu_br_u8(&z->br);
    x = djvu_br_s16be_biased(&z->br);
    y = djvu_br_s16be_biased(&z->br);
    w = djvu_br_s16be_biased(&z->br);
    h = djvu_br_s16be_biased(&z->br);
    toff = djvu_br_s16be_biased(&z->br);
    tlen = djvu_br_u24be(&z->br);
    if (z->br.failed) return -1;

    /* ResolveOffsets (DjvuNet TextZone.ResolveOffsets), bottom-up coords */
    if (parent == NULL && sib == NULL) {
        /* absolute (page zone) */
    } else if (sib == NULL) {
        x += parent->x;
        y = (parent->y + parent->h) - (y + h);
        toff += parent_toff;
    } else {
        if (sib->type == DJVU_ZONE_PAGE || sib->type == DJVU_ZONE_PARAGRAPH ||
            sib->type == DJVU_ZONE_LINE) {
            x += sib->x;
            y = sib->y - (y + h);
        } else if (sib->type == DJVU_ZONE_COLUMN || sib->type == DJVU_ZONE_WORD ||
                   sib->type == DJVU_ZONE_CHAR) {
            x += sib->x + sib->w;
            y += sib->y;
        }
        /* (Region siblings leave x,y unadjusted, matching DjvuNet) */
        toff += sib_toff + sib_tlen;
    }

    out->type = (djvu_zone_type)type;
    out->x = x; out->y = y; out->w = w; out->h = h;
    out->text = zone_text(z, toff, tlen);
    out->children = NULL;
    out->nchildren = 0;
    if (out_toff) *out_toff = toff;
    if (out_tlen) *out_tlen = tlen;

    nkids = djvu_br_u24be(&z->br);
    if (z->br.failed || nkids < 0) return -1;
    if (nkids > 0) {
        out->children = (djvu_text_zone *)djvu_alloc(z->br.ctx,
                            sizeof(djvu_text_zone) * (size_t)nkids);
        if (!out->children) return -1;
        memset(out->children, 0, sizeof(djvu_text_zone) * (size_t)nkids);
        for (i = 0; i < nkids; i++) {
            int kt = 0, kl = 0;
            if (parse_zone(z, &out->children[i], out, prev,
                           toff, prev_toff, prev_tlen, &kt, &kl) != 0) {
                out->nchildren = i;   /* free what we built */
                return -1;
            }
            prev = &out->children[i];
            prev_toff = kt; prev_tlen = kl;
        }
        out->nchildren = nkids;
    }
    return 0;
}

/* Recursively flip y from bottom-up (DjVu) to top-down (image) coords. */
static void flip_zone_y(djvu_text_zone *z, int page_h)
{
    int i;
    z->y = djvu_y_bottomup_to_topdown(z->y, page_h, z->h);
    for (i = 0; i < z->nchildren; i++) flip_zone_y(&z->children[i], page_h);
}

static void free_zone_kids(djvu_ctx *ctx, djvu_text_zone *z)
{
    int i;
    for (i = 0; i < z->nchildren; i++) free_zone_kids(ctx, &z->children[i]);
    djvu_free(ctx, z->children);
    djvu_free(ctx, z->text);
}

djvu_page_text_zones *djvu_page_text_get_zones(djvu_doc *doc, int page_no)
{
    djvu_ctx *ctx;
    uint8_t *owned = NULL;
    const uint8_t *payload;
    size_t plen;
    uint32_t tlen;
    zparse z;
    djvu_page_text_zones *res;
    djvu_page_info info;
    int page_h;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    if (load_text_payload(doc, page_no, &owned, &payload, &plen) != 0) return NULL;
    if (plen < 4) { djvu_free(ctx, owned); return NULL; }

    tlen = djvu_rd_u24be(payload);
    if ((size_t)tlen + 3 > plen) tlen = (uint32_t)(plen - 3);

    res = (djvu_page_text_zones *)djvu_alloc(ctx, sizeof(*res));
    if (!res) { djvu_free(ctx, owned); return NULL; }
    res->text = (char *)djvu_alloc(ctx, tlen + 1);
    res->root = NULL;
    if (!res->text) { djvu_free(ctx, owned); djvu_free(ctx, res); return NULL; }
    memcpy(res->text, payload + 3, tlen);
    res->text[tlen] = 0;

    /* after text: version byte, then the root (page) zone */
    memset(&z, 0, sizeof(z));
    djvu_br_init(&z.br, ctx, payload, plen);
    z.br.pos = (size_t)3 + tlen + 1;   /* skip length, text, version byte */
    z.text = res->text;
    z.textlen = tlen;

    if (z.br.pos < plen) {
        djvu_text_zone *root = (djvu_text_zone *)djvu_alloc(ctx, sizeof(*root));
        if (root) {
            memset(root, 0, sizeof(*root));
            if (parse_zone(&z, root, NULL, NULL, 0, 0, 0, NULL, NULL) == 0) {
                page_h = (djvu_doc_page_info(doc, page_no, &info) == 0)
                             ? info.height : (root->y + root->h);
                flip_zone_y(root, page_h);
                res->root = root;
            } else {
                free_zone_kids(ctx, root);
                djvu_free(ctx, root);
            }
        }
    }

    djvu_free(ctx, owned);
    return res;
}

void djvu_text_zones_destroy(djvu_ctx *ctx, djvu_page_text_zones *z)
{
    if (!z) return;
    if (z->root) {
        free_zone_kids(ctx, z->root);
        djvu_free(ctx, z->root);
    }
    djvu_free(ctx, z->text);
    djvu_free(ctx, z);
}
