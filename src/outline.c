/* outline.c -- document outline / bookmarks (NAVM chunk).
 * Ported from DjvuNet DataChunks/NavmChunk.cs + Navigation/Bookmark.cs.
 * NAVM is BZZ-compressed: u16-BE total bookmark count, then a forest of
 * bookmarks; each = u8 child-count, u24-BE name length + UTF-8 name,
 * u24-BE url length + UTF-8 url, then child-count children. */
#include "djvu_internal.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    djvu_ctx *ctx;
    djvu_buf_reader br;
    int count;       /* total bookmark nodes parsed so far */
} nparse;

/* Resolve a bookmark url to a 0-based page number, or -1. */
static int resolve_url(djvu_doc *doc, const char *url)
{
    const char *s = url;
    int allnum = 1, n;
    if (!s || !*s) return -1;
    if (*s == '#') s++;
    if (!*s) return -1;
    { const char *t = s; for (; *t; t++) if (*t < '0' || *t > '9') { allnum = 0; break; } }
    if (allnum) {
        n = atoi(s) - 1;   /* DjVu page links are 1-based */
        if (n >= 0 && n < doc->npages) return n;
        return -1;
    }
    return djvu_doc_page_by_name(doc, s);
}

static void free_items(djvu_ctx *ctx, djvu_outline_item *items, int n);

/* Parse one bookmark (and its subtree) into *out. Returns 0 on success. */
static int parse_item(nparse *np, djvu_doc *doc, djvu_outline_item *out)
{
    int nkids, namelen, urllen, i;

    np->count++;
    nkids = djvu_br_u8(&np->br);
    namelen = djvu_br_u24be(&np->br);
    out->title = djvu_br_strdup(&np->br, namelen);
    urllen = djvu_br_u24be(&np->br);
    out->url = djvu_br_strdup(&np->br, urllen);
    out->page_no = out->url ? resolve_url(doc, out->url) : -1;
    out->children = NULL;
    out->nchildren = 0;
    if (np->br.failed) return -1;

    if (nkids > 0) {
        out->children = (djvu_outline_item *)djvu_alloc(np->ctx,
                            sizeof(djvu_outline_item) * (size_t)nkids);
        if (!out->children) return -1;
        memset(out->children, 0, sizeof(djvu_outline_item) * (size_t)nkids);
        for (i = 0; i < nkids; i++) {
            if (parse_item(np, doc, &out->children[i]) != 0) {
                out->nchildren = i;
                return -1;
            }
        }
        out->nchildren = nkids;
    }
    return 0;
}

static void free_items(djvu_ctx *ctx, djvu_outline_item *items, int n)
{
    int i;
    if (!items) return;
    for (i = 0; i < n; i++) {
        free_items(ctx, items[i].children, items[i].nchildren);
        djvu_free(ctx, items[i].title);
        djvu_free(ctx, items[i].url);
    }
    djvu_free(ctx, items);
}

djvu_outline_item *djvu_doc_outline(djvu_doc *doc)
{
    djvu_ctx *ctx;
    const uint8_t *navm;
    uint32_t sz;
    uint8_t *dec;
    size_t dlen;
    nparse np;
    int total, cap, n;
    djvu_outline_item *items = NULL, *root;

    if (!doc) return NULL;
    ctx = doc->ctx;
    navm = djvu_form_find_chunk(doc, doc->root_form_off, "NAVM", &sz, NULL);
    if (!navm) return NULL;

    dec = djvu_bzz_decode_all(ctx, navm, sz, &dlen);
    if (!dec) return NULL;

    memset(&np, 0, sizeof(np));
    np.ctx = ctx;
    djvu_br_init(&np.br, ctx, dec, dlen);
    total = djvu_br_u16be(&np.br);
    if (np.br.failed || total <= 0) { djvu_free(ctx, dec); return NULL; }

    cap = 0; n = 0;
    while (!np.br.failed && np.br.pos < dlen && np.count < total) {
        djvu_outline_item tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (parse_item(&np, doc, &tmp) != 0) {
            free_items(ctx, tmp.children, tmp.nchildren);
            djvu_free(ctx, tmp.title); djvu_free(ctx, tmp.url);
            break;
        }
        if (n == cap) {
            int ncap = cap ? cap * 2 : 8;
            djvu_outline_item *na = (djvu_outline_item *)djvu_alloc(ctx,
                                        sizeof(djvu_outline_item) * (size_t)ncap);
            if (!na) { free_items(ctx, tmp.children, tmp.nchildren);
                       djvu_free(ctx, tmp.title); djvu_free(ctx, tmp.url); break; }
            if (items) { memcpy(na, items, sizeof(djvu_outline_item) * (size_t)n);
                         djvu_free(ctx, items); }
            items = na; cap = ncap;
        }
        items[n++] = tmp;
    }

    djvu_free(ctx, dec);

    if (n == 0) { djvu_free(ctx, items); return NULL; }

    root = (djvu_outline_item *)djvu_alloc(ctx, sizeof(*root));
    if (!root) { free_items(ctx, items, n); return NULL; }
    memset(root, 0, sizeof(*root));
    root->page_no = -1;
    root->children = items;
    root->nchildren = n;
    return root;
}

void djvu_outline_destroy(djvu_ctx *ctx, djvu_outline_item *root)
{
    if (!root) return;
    free_items(ctx, root->children, root->nchildren);
    djvu_free(ctx, root);
}