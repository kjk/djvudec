/* annot.c -- page hyperlinks from annotation chunks (ANTa raw / ANTz BZZ).
 * Annotations are a DjVuLibre S-expression list; hyperlinks are (maparea ...)
 * forms: (maparea url comment (shape x y w h ...) ...). Mirrors what
 * ddjvu_anno_get_hyperlinks extracts (rect/oval/text shapes). */
#include "djvu_internal.h"
#include <string.h>
#include <stdlib.h>

/* ---- tiny S-expression parser ---- */

typedef struct snode {
    int kind;            /* 0 = list, 1 = atom (symbol/number/string) */
    char *text;          /* atom value (decoded), NUL-terminated */
    struct snode **kids; /* list children */
    int nkids, cap;
} snode;

typedef struct {
    djvu_ctx *ctx;
    const char *s;
    size_t len, pos;
} sparse;

static void snode_free(djvu_ctx *ctx, snode *n)
{
    int i;
    if (!n) return;
    for (i = 0; i < n->nkids; i++) snode_free(ctx, n->kids[i]);
    djvu_free(ctx, n->kids);
    djvu_free(ctx, n->text);
    djvu_free(ctx, n);
}

static void snode_add(sparse *sp, snode *list, snode *kid)
{
    if (list->nkids == list->cap) {
        int nc = list->cap ? list->cap * 2 : 8;
        snode **na = (snode **)djvu_alloc(sp->ctx, sizeof(snode *) * (size_t)nc);
        if (!na) { snode_free(sp->ctx, kid); return; }
        if (list->kids) {
            memcpy(na, list->kids, sizeof(snode *) * (size_t)list->nkids);
            djvu_free(sp->ctx, list->kids);
        }
        list->kids = na; list->cap = nc;
    }
    list->kids[list->nkids++] = kid;
}

static void skip_ws(sparse *sp)
{
    while (sp->pos < sp->len) {
        char c = sp->s[sp->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f')
            sp->pos++;
        else
            break;
    }
}

/* Parse a quoted string starting at the opening '"'. */
static snode *parse_string(sparse *sp)
{
    djvu_ctx *ctx = sp->ctx;
    size_t cap = 16, n = 0;
    char *buf = (char *)djvu_alloc(ctx, cap);
    snode *node;
    if (!buf) return NULL;
    sp->pos++;   /* opening quote */
    while (sp->pos < sp->len) {
        char c = sp->s[sp->pos++];
        if (c == '"') break;
        if (c == '\\' && sp->pos < sp->len) {
            char e = sp->s[sp->pos++];
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'f': c = '\f'; break;
                case 'b': c = '\b'; break;
                default:  c = e;    break;   /* \" \\ and any other */
            }
        }
        if (n + 1 >= cap) {
            char *nb = (char *)djvu_alloc(ctx, cap * 2);
            if (!nb) { djvu_free(ctx, buf); return NULL; }
            memcpy(nb, buf, n); djvu_free(ctx, buf); buf = nb; cap *= 2;
        }
        buf[n++] = c;
    }
    buf[n] = 0;
    node = (snode *)djvu_alloc(ctx, sizeof(snode));
    if (!node) { djvu_free(ctx, buf); return NULL; }
    memset(node, 0, sizeof(*node));
    node->kind = 1; node->text = buf;
    return node;
}

/* Parse a bare symbol/number token. */
static snode *parse_atom(sparse *sp)
{
    djvu_ctx *ctx = sp->ctx;
    size_t start = sp->pos;
    snode *node;
    char *buf;
    size_t n;
    while (sp->pos < sp->len) {
        char c = sp->s[sp->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '(' || c == ')' || c == '"')
            break;
        sp->pos++;
    }
    n = sp->pos - start;
    buf = (char *)djvu_alloc(ctx, n + 1);
    if (!buf) return NULL;
    memcpy(buf, sp->s + start, n); buf[n] = 0;
    node = (snode *)djvu_alloc(ctx, sizeof(snode));
    if (!node) { djvu_free(ctx, buf); return NULL; }
    memset(node, 0, sizeof(*node));
    node->kind = 1; node->text = buf;
    return node;
}

static snode *parse_node(sparse *sp);

static snode *parse_list(sparse *sp)
{
    djvu_ctx *ctx = sp->ctx;
    snode *list = (snode *)djvu_alloc(ctx, sizeof(snode));
    if (!list) return NULL;
    memset(list, 0, sizeof(*list));
    list->kind = 0;
    sp->pos++;   /* opening paren */
    for (;;) {
        skip_ws(sp);
        if (sp->pos >= sp->len) break;
        if (sp->s[sp->pos] == ')') { sp->pos++; break; }
        {
            snode *kid = parse_node(sp);
            if (!kid) break;
            snode_add(sp, list, kid);
        }
    }
    return list;
}

static snode *parse_node(sparse *sp)
{
    skip_ws(sp);
    if (sp->pos >= sp->len) return NULL;
    if (sp->s[sp->pos] == '(') return parse_list(sp);
    if (sp->s[sp->pos] == '"') return parse_string(sp);
    if (sp->s[sp->pos] == ')') return NULL;
    return parse_atom(sp);
}

/* ---- link extraction ---- */

static const char *atom_text(const snode *n)
{
    return (n && n->kind == 1) ? n->text : NULL;
}
static int is_list_head(const snode *n, const char *head)
{
    return n && n->kind == 0 && n->nkids > 0 &&
           atom_text(n->kids[0]) && strcmp(atom_text(n->kids[0]), head) == 0;
}

typedef struct {
    djvu_ctx *ctx;
    djvu_doc *doc;
    int page_h;
    djvu_link *links;
    int n, cap;
} lcollect;

static char *dup_str(djvu_ctx *ctx, const char *s)
{
    size_t n;
    char *r;
    if (!s) return NULL;
    n = strlen(s);
    r = (char *)djvu_alloc(ctx, n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static void collect_maparea(lcollect *lc, const snode *area)
{
    const snode *url_node = NULL, *comment_node = NULL, *shape = NULL;
    const char *url = NULL, *comment = NULL, *head;
    djvu_link_shape stype;
    int i, x, y, w, h;

    /* maparea kids: head, url, comment, shape, [effects...] (order varies) */
    for (i = 1; i < area->nkids; i++) {
        const snode *k = area->kids[i];
        if (!shape && k->kind == 0) {
            head = atom_text(k->kids ? k->kids[0] : NULL);
            if (head && (!strcmp(head, "rect") || !strcmp(head, "oval") ||
                         !strcmp(head, "text") || !strcmp(head, "poly") ||
                         !strcmp(head, "line"))) {
                shape = k; continue;
            }
        }
        if (!url_node) { url_node = k; continue; }
        if (!comment_node && k->kind == 1) { comment_node = k; continue; }
    }

    if (url_node) {
        if (url_node->kind == 1) url = url_node->text;
        else if (is_list_head(url_node, "url") && url_node->nkids >= 2)
            url = atom_text(url_node->kids[1]);
    }
    if (comment_node) comment = atom_text(comment_node);
    if (!url || !*url || !shape) return;

    head = atom_text(shape->kids[0]);
    if (!strcmp(head, "oval")) stype = DJVU_LINK_OVAL;
    else if (!strcmp(head, "text")) stype = DJVU_LINK_TEXT;
    else if (!strcmp(head, "rect")) stype = DJVU_LINK_RECT;
    else return;   /* poly/line: no axis-aligned box (matches SumatraPDF) */

    if (shape->nkids < 5) return;
    x = atoi(atom_text(shape->kids[1]) ? atom_text(shape->kids[1]) : "0");
    y = atoi(atom_text(shape->kids[2]) ? atom_text(shape->kids[2]) : "0");
    w = atoi(atom_text(shape->kids[3]) ? atom_text(shape->kids[3]) : "0");
    h = atoi(atom_text(shape->kids[4]) ? atom_text(shape->kids[4]) : "0");

    if (lc->n == lc->cap) {
        int nc = lc->cap ? lc->cap * 2 : 8;
        djvu_link *na = (djvu_link *)djvu_alloc(lc->ctx, sizeof(djvu_link) * (size_t)nc);
        if (!na) return;
        if (lc->links) {
            memcpy(na, lc->links, sizeof(djvu_link) * (size_t)lc->n);
            djvu_free(lc->ctx, lc->links);
        }
        lc->links = na; lc->cap = nc;
    }
    {
        djvu_link *L = &lc->links[lc->n++];
        L->url = dup_str(lc->ctx, url);
        L->comment = (comment && *comment) ? dup_str(lc->ctx, comment) : NULL;
        L->shape = stype;
        L->x = x;
        L->y = djvu_y_bottomup_to_topdown(y, lc->page_h, h);
        L->w = w;
        L->h = h;
    }
}

static void walk(lcollect *lc, const snode *n)
{
    int i;
    if (!n || n->kind != 0) return;
    if (is_list_head(n, "maparea")) collect_maparea(lc, n);
    for (i = 0; i < n->nkids; i++) walk(lc, n->kids[i]);
}

/* Find the annotation buffer for a page: page form first, else a shared DJVI
   component referenced by INCL. Returns a freshly allocated NUL-terminated
   buffer (set *owned), or NULL. */
static char *load_anno(djvu_doc *doc, uint32_t form_off, size_t *out_len)
{
    djvu_ctx *ctx = doc->ctx;
    uint32_t sz;
    const uint8_t *chunk;

    chunk = djvu_form_find_chunk(doc, form_off, "ANTz", &sz, NULL);
    if (chunk) {
        uint8_t *dec = djvu_bzz_decode_all(ctx, chunk, sz, out_len);
        return (char *)dec;
    }
    chunk = djvu_form_find_chunk(doc, form_off, "ANTa", &sz, NULL);
    if (chunk) {
        char *buf = (char *)djvu_alloc(ctx, sz + 1);
        if (!buf) return NULL;
        memcpy(buf, chunk, sz); buf[sz] = 0;
        *out_len = sz;
        return buf;
    }
    chunk = djvu_form_find_incl_chunk(doc, form_off, "ANTz", &sz);
    if (chunk)
        return (char *)djvu_bzz_decode_all(ctx, chunk, sz, out_len);
    chunk = djvu_form_find_incl_chunk(doc, form_off, "ANTa", &sz);
    if (chunk) {
        char *buf = (char *)djvu_alloc(ctx, sz + 1);
        if (!buf) return NULL;
        memcpy(buf, chunk, sz); buf[sz] = 0;
        *out_len = sz;
        return buf;
    }
    return NULL;
}

djvu_page_links *djvu_page_get_links(djvu_doc *doc, int page_no)
{
    djvu_ctx *ctx;
    char *buf;
    size_t blen = 0;
    sparse sp;
    lcollect lc;
    djvu_page_info info;
    djvu_page_links *res;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    buf = load_anno(doc, doc->pages[page_no].form_off, &blen);
    if (!buf) return NULL;

    memset(&lc, 0, sizeof(lc));
    lc.ctx = ctx; lc.doc = doc;
    lc.page_h = (djvu_doc_page_info(doc, page_no, &info) == 0) ? info.height : 0;

    memset(&sp, 0, sizeof(sp));
    sp.ctx = ctx; sp.s = buf; sp.len = blen;
    /* the annotation body is a sequence of top-level forms */
    while (sp.pos < sp.len) {
        snode *node;
        skip_ws(&sp);
        if (sp.pos >= sp.len || sp.s[sp.pos] != '(') { sp.pos++; continue; }
        node = parse_node(&sp);
        if (!node) break;
        walk(&lc, node);
        snode_free(ctx, node);
    }
    djvu_free(ctx, buf);

    if (lc.n == 0) { djvu_free(ctx, lc.links); return NULL; }

    res = (djvu_page_links *)djvu_alloc(ctx, sizeof(*res));
    if (!res) {
        int i;
        for (i = 0; i < lc.n; i++) { djvu_free(ctx, lc.links[i].url);
                                     djvu_free(ctx, lc.links[i].comment); }
        djvu_free(ctx, lc.links);
        return NULL;
    }
    res->links = lc.links;
    res->nlinks = lc.n;
    return res;
}

void djvu_page_links_destroy(djvu_ctx *ctx, djvu_page_links *links)
{
    int i;
    if (!links) return;
    for (i = 0; i < links->nlinks; i++) {
        djvu_free(ctx, links->links[i].url);
        djvu_free(ctx, links->links[i].comment);
    }
    djvu_free(ctx, links->links);
    djvu_free(ctx, links);
}
