/* document.c -- context, document open/close, IFF/DJVM/DIRM/INFO parsing.
 * Ported from DjvuNet Parser/DjvuParser.cs + DataChunks/{DirmChunk,InfoChunk}.cs */
#include "djvu_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---- allocator + diagnostics ---- */

static void *default_alloc(void *user, size_t size) { (void)user; return malloc(size); }
static void  default_free(void *user, void *ptr) { (void)user; free(ptr); }

void *djvu_alloc(djvu_ctx *ctx, size_t size) { return ctx->alloc(ctx->user, size); }
void  djvu_free(djvu_ctx *ctx, void *ptr) { if (ptr) ctx->free(ctx->user, ptr); }

void djvu_errorf(djvu_ctx *ctx, djvu_severity sev, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!ctx->error) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->error(ctx->user, sev, buf);
}

djvu_ctx *djvu_ctx_new(djvu_alloc_cb alloc, djvu_free_cb free_cb,
                       djvu_error_cb error, void *user)
{
    djvu_ctx *ctx;
    djvu_alloc_cb a = alloc ? alloc : default_alloc;
    ctx = (djvu_ctx *)a(user, sizeof(djvu_ctx));
    if (!ctx) return NULL;
    ctx->alloc = a;
    ctx->free = free_cb ? free_cb : default_free;
    ctx->error = error;
    ctx->user = user;
    return ctx;
}

void djvu_ctx_free(djvu_ctx *ctx)
{
    if (ctx) ctx->free(ctx->user, ctx);
}

/* ---- INFO chunk ---- */

/* Parse an INFO chunk body at [p, p+len) into info. Returns 0 on success. */
static int parse_info(const uint8_t *p, size_t len, djvu_page_info *info)
{
    int flag;
    if (len < 5) return -1;
    info->width = (int)djvu_rd_u16be(p);
    info->height = (int)djvu_rd_u16be(p + 2);
    info->version = p[4];                       /* minor version */
    info->dpi = 300;
    info->rotation = 0;
    if (len >= 8) info->dpi = (int)djvu_rd_u16le(p + 6);  /* DPI is little-endian */
    if (len >= 10) {
        flag = p[9] & 0x7;
        switch (flag) {
            case 6: info->rotation = 90; break;   /* counter-clockwise */
            case 2: info->rotation = 180; break;  /* upside down */
            case 5: info->rotation = 270; break;  /* clockwise */
            default: info->rotation = 0; break;
        }
    }
    if (info->dpi <= 0) info->dpi = 300;
    return 0;
}

/* Find the INFO chunk inside a FORM:DJVU at form_off and fill info.
   Returns 0 on success. */
static int page_load_info(djvu_doc *doc, djvu_page_int *pg)
{
    const uint8_t *data = doc->data;
    size_t len = doc->len;
    uint32_t off = pg->form_off;
    uint32_t form_end;
    uint32_t pos;

    if (pg->has_info) return 0;
    if ((size_t)off + 12 > len) return -1;
    if (!djvu_tag_eq(data + off, "FORM")) return -1;
    form_end = off + 8 + pg->form_size;
    if (form_end > len) form_end = (uint32_t)len;

    /* sub-chunks begin after FORM(4) + size(4) + type(4) */
    pos = off + 12;
    while (pos + 8 <= form_end) {
        const uint8_t *id = data + pos;
        uint32_t csize = djvu_rd_u32be(data + pos + 4);
        uint32_t cdata = pos + 8;
        if (cdata + csize > form_end) csize = form_end - cdata;
        if (djvu_tag_eq(id, "INFO")) {
            if (parse_info(data + cdata, csize, &pg->info) == 0) {
                pg->has_info = 1;
                return 0;
            }
            return -1;
        }
        pos = cdata + csize;
        if (csize & 1) pos++;   /* chunks padded to even length */
    }
    return -1;
}

/* ---- container ---- */

static char *dup_cstr(djvu_ctx *ctx, const char *s, size_t maxlen, size_t *consumed)
{
    size_t n = 0;
    char *r;
    while (n < maxlen && s[n]) n++;
    r = (char *)djvu_alloc(ctx, n + 1);
    if (r) { memcpy(r, s, n); r[n] = 0; }
    *consumed = (n < maxlen) ? n + 1 : n;   /* skip the NUL if present */
    return r;
}

/* Parse a DJVM directory (DIRM): component offsets + BZZ-compressed
   sizes/flags/ids. Builds doc->comps and doc->pages. */
static int load_djvm(djvu_doc *doc, uint32_t dirm_data, uint32_t dirm_size)
{
    djvu_ctx *ctx = doc->ctx;
    const uint8_t *data = doc->data;
    size_t len = doc->len;
    uint32_t pos = dirm_data;
    uint32_t dirm_end = dirm_data + dirm_size;
    int count, i, npages = 0;
    int version, bundled;
    uint8_t flagbyte;
    uint8_t *dir = NULL;       /* decompressed directory */
    size_t dirlen = 0, dp = 0;
    int have_dir = 0;

    if (pos + 3 > dirm_end || dirm_end > len) return -1;
    flagbyte = data[pos++];
    bundled = (flagbyte >> 7) & 1;
    version = flagbyte & 0x7f;
    count = (int)djvu_rd_u16be(data + pos);
    pos += 2;
    if (count <= 0) return -1;

    doc->comps = (djvu_component *)djvu_alloc(ctx, sizeof(djvu_component) * count);
    doc->pages = (djvu_page_int *)djvu_alloc(ctx, sizeof(djvu_page_int) * count);
    if (!doc->comps || !doc->pages) return -1;
    memset(doc->comps, 0, sizeof(djvu_component) * count);
    memset(doc->pages, 0, sizeof(djvu_page_int) * count);
    doc->ncomp = count;

    /* component offsets (bundled documents only) */
    if (bundled) {
        if ((size_t)pos + (size_t)count * 4 > len) return -1;
        for (i = 0; i < count; i++)
            doc->comps[i].offset = djvu_rd_u32be(data + pos + (uint32_t)i * 4);
        pos += (uint32_t)count * 4;
    }

    /* BZZ-compressed section: sizes, flags, ids */
    if (pos < dirm_end) {
        dir = djvu_bzz_decode_all(ctx, data + pos, dirm_end - pos, &dirlen);
    }
    if (dir) {
        size_t flags_off, sp;
        /* sizes: u24 BE * count */
        for (i = 0; i < count && dp + 3 <= dirlen; i++, dp += 3)
            doc->comps[i].size = djvu_rd_u24be(dir + dp);
        /* flags: u8 * count */
        flags_off = dp;
        for (i = 0; i < count && dp < dirlen; i++, dp++) {
            uint8_t fl = dir[dp];
            doc->comps[i].type = (version == 0) ? ((fl & 0x01) ? 1 : 0)
                                                : (fl & 0x3f);
        }
        /* strings: id [name] [title] per component */
        sp = dp;
        for (i = 0; i < count && sp < dirlen; i++) {
            uint8_t fl = (flags_off + (size_t)i < dirlen) ? dir[flags_off + i] : 0;
            int has_name, has_title;
            size_t used;
            if (version == 0) {
                has_name = (fl & 0x02) != 0; has_title = (fl & 0x04) != 0;
            } else {
                has_name = (fl & 0x80) != 0; has_title = (fl & 0x40) != 0;
            }
            doc->comps[i].id = dup_cstr(ctx, (char *)dir + sp, dirlen - sp, &used);
            sp += used;
            if (has_name && sp < dirlen) {
                char *nm = dup_cstr(ctx, (char *)dir + sp, dirlen - sp, &used);
                djvu_free(ctx, nm); sp += used;   /* name (file name) unused */
            }
            if (has_title && sp < dirlen) {
                doc->comps[i].title = dup_cstr(ctx, (char *)dir + sp, dirlen - sp, &used);
                sp += used;
            }
        }
        djvu_free(ctx, dir);
        dir = NULL;
        have_dir = 1;
    }

    /* build page list: components of type "page" (or, fallback, FORM:DJVU). */
    for (i = 0; i < count; i++) {
        uint32_t o = doc->comps[i].offset;
        int is_page = (doc->comps[i].type == 1);
        if (!bundled) continue;
        if ((size_t)o + 12 > len || !djvu_tag_eq(data + o, "FORM")) continue;
        if (!have_dir) is_page = djvu_tag_eq(data + o + 8, "DJVU");
        if (!is_page) continue;
        doc->pages[npages].form_off = o;
        doc->pages[npages].form_size = djvu_rd_u32be(data + o + 4);
        doc->pages[npages].id = doc->comps[i].id;
        doc->pages[npages].title = doc->comps[i].title;
        npages++;
    }
    doc->npages = npages;
    return 0;
}

/* ---- IW44 cache (eager decode at doc open; immutable during render) ---- */

static void free_page_iw44(djvu_ctx *ctx, djvu_page_int *pg)
{
    if (pg->iw_bg) { djvu_iw44_free(pg->iw_bg); pg->iw_bg = NULL; }
    if (pg->iw_fg) { djvu_iw44_free(pg->iw_fg); pg->iw_fg = NULL; }
}

static void preload_iw_layer(djvu_doc *doc, djvu_page_int *pg, const char *id,
                             iw_pixmap **slot)
{
    uint32_t sz;
    const char *mc;
    int maxc;
    iw_pixmap *pm;

    if (*slot || !djvu_form_find_chunk(doc, pg->form_off, id, &sz, NULL))
        return;
    pm = djvu_iw44_new(doc->ctx);
    if (!pm) return;
    mc = getenv("DJVU_IW_MAXCHUNKS");
    maxc = mc ? atoi(mc) : 0;
    if (djvu_iw44_decode_form(doc, pg->form_off, id, pm, maxc) != 0) {
        djvu_iw44_free(pm);
        djvu_errorf(doc->ctx, DJVU_SEVERITY_WARNING,
                    "IW44 preload failed (%s at form %u)", id, pg->form_off);
        return;
    }
    *slot = pm;
}

static void djvu_doc_preload_iw44(djvu_doc *doc)
{
    int i;
    for (i = 0; i < doc->npages; i++) {
        djvu_page_int *pg = &doc->pages[i];
        preload_iw_layer(doc, pg, "BG44", &pg->iw_bg);
        preload_iw_layer(doc, pg, "FG44", &pg->iw_fg);
    }
}

iw_pixmap *djvu_doc_iw44(djvu_doc *doc, int page_no, const char *chunk_id)
{
    if (!doc || page_no < 0 || page_no >= doc->npages || !chunk_id) return NULL;
    if (chunk_id[0] == 'B' && chunk_id[1] == 'G') return doc->pages[page_no].iw_bg;
    if (chunk_id[0] == 'F' && chunk_id[1] == 'G') return doc->pages[page_no].iw_fg;
    return NULL;
}

iw_pixmap *djvu_doc_iw44_by_form(djvu_doc *doc, uint32_t form_off, const char *chunk_id)
{
    int i;
    if (!doc) return NULL;
    for (i = 0; i < doc->npages; i++)
        if (doc->pages[i].form_off == form_off)
            return djvu_doc_iw44(doc, i, chunk_id);
    return NULL;
}

/* ---- JB2 shared-dict cache (eager decode at doc open; immutable during render) ---- */

static jb2_image *jb2_dict_find(djvu_doc *doc, const char *incl_id)
{
    int i;
    if (!doc || !incl_id || !incl_id[0]) return NULL;
    for (i = 0; i < doc->n_jb2_dicts; i++)
        if (doc->jb2_dicts[i].incl_id &&
            strcmp(doc->jb2_dicts[i].incl_id, incl_id) == 0)
            return doc->jb2_dicts[i].dict;
    return NULL;
}

static void jb2_dict_cache_add(djvu_doc *doc, const char *incl_id, jb2_image *dict)
{
    djvu_jb2_dict_entry *e;
    int n = doc->n_jb2_dicts + 1;
    char *idcopy;

    if (!doc || !incl_id || !incl_id[0] || !dict || jb2_dict_find(doc, incl_id))
        return;
    idcopy = (char *)djvu_alloc(doc->ctx, strlen(incl_id) + 1);
    if (!idcopy) return;
    strcpy(idcopy, incl_id);
    e = (djvu_jb2_dict_entry *)djvu_alloc(doc->ctx, sizeof(djvu_jb2_dict_entry) * n);
    if (!e) { djvu_free(doc->ctx, idcopy); return; }
    if (doc->jb2_dicts) {
        memcpy(e, doc->jb2_dicts, sizeof(djvu_jb2_dict_entry) * doc->n_jb2_dicts);
        djvu_free(doc->ctx, doc->jb2_dicts);
    }
    doc->jb2_dicts = e;
    doc->jb2_dicts[doc->n_jb2_dicts].incl_id = idcopy;
    doc->jb2_dicts[doc->n_jb2_dicts].dict = dict;
    doc->n_jb2_dicts = n;
}

static void preload_jb2_dict_incl(djvu_doc *doc, const char *incl_id)
{
    uint32_t coff, sz;
    const uint8_t *djbz;
    jb2_image *dict;

    if (!doc || !incl_id || !incl_id[0] || jb2_dict_find(doc, incl_id))
        return;
    coff = djvu_doc_component_offset(doc, incl_id);
    if (!coff) return;
    djbz = djvu_form_find_chunk(doc, coff, "Djbz", &sz, NULL);
    if (!djbz) return;
    dict = djvu_jb2_decode_dict(doc->ctx, djbz, sz);
    if (!dict) {
        djvu_errorf(doc->ctx, DJVU_SEVERITY_WARNING,
                    "JB2 dict preload failed (INCL %s)", incl_id);
        return;
    }
    jb2_dict_cache_add(doc, incl_id, dict);
}

static void preload_jb2_dicts_from_page(djvu_doc *doc, uint32_t form_off)
{
    uint32_t start = 0, incl_sz;
    const uint8_t *incl;

    while ((incl = djvu_form_find_chunk(doc, form_off, "INCL", &incl_sz, &start)) != NULL) {
        char id[64];
        size_t n = incl_sz < sizeof(id) - 1 ? incl_sz : sizeof(id) - 1;
        memcpy(id, incl, n);
        id[n] = 0;
        djvu_trim_incl_id(id);
        preload_jb2_dict_incl(doc, id);
    }
}

static void djvu_doc_preload_jb2_dicts(djvu_doc *doc)
{
    int i;
    if (!doc) return;
    for (i = 0; i < doc->ncomp; i++)
        if (doc->comps[i].type == 0 && doc->comps[i].id)
            preload_jb2_dict_incl(doc, doc->comps[i].id);
    for (i = 0; i < doc->npages; i++)
        preload_jb2_dicts_from_page(doc, doc->pages[i].form_off);
}

static void free_jb2_dict_cache(djvu_ctx *ctx, djvu_doc *doc)
{
    int i;
    if (!doc || !doc->jb2_dicts) return;
    for (i = 0; i < doc->n_jb2_dicts; i++) {
        djvu_jb2_free(ctx, doc->jb2_dicts[i].dict);
        djvu_free(ctx, doc->jb2_dicts[i].incl_id);
    }
    djvu_free(ctx, doc->jb2_dicts);
    doc->jb2_dicts = NULL;
    doc->n_jb2_dicts = 0;
}

jb2_image *djvu_doc_jb2_dict(djvu_doc *doc, const char *incl_id)
{
    return jb2_dict_find(doc, incl_id);
}

jb2_image *djvu_doc_jb2_dict_for_form(djvu_doc *doc, uint32_t form_off)
{
    uint32_t start = 0, incl_sz, chunk_sz;
    const uint8_t *incl;
    jb2_image *dict;

    if (!doc) return NULL;
    if (djvu_form_find_chunk(doc, form_off, "Djbz", &chunk_sz, NULL))
        return NULL;
    while ((incl = djvu_form_find_chunk(doc, form_off, "INCL", &incl_sz, &start)) != NULL) {
        char id[64];
        uint32_t coff;
        const uint8_t *djbz;
        size_t n = incl_sz < sizeof(id) - 1 ? incl_sz : sizeof(id) - 1;
        memcpy(id, incl, n);
        id[n] = 0;
        djvu_trim_incl_id(id);
        dict = jb2_dict_find(doc, id);
        if (dict) return dict;
        coff = djvu_doc_component_offset(doc, id);
        if (!coff) continue;
        djbz = djvu_form_find_chunk(doc, coff, "Djbz", &chunk_sz, NULL);
        if (djbz) return NULL; /* INCL has Djbz but cache miss (preload failed) */
    }
    return NULL;
}

uint32_t djvu_doc_component_offset(djvu_doc *doc, const char *id)
{
    int i;
    if (!doc || !id) return 0;
    for (i = 0; i < doc->ncomp; i++)
        if (doc->comps[i].id && strcmp(doc->comps[i].id, id) == 0)
            return doc->comps[i].offset;
    return 0;
}

void djvu_trim_incl_id(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == 0x1a))
        s[--n] = 0;
}

const uint8_t *djvu_form_find_incl_chunk(djvu_doc *doc, uint32_t form_off,
                                         const char *chunk_id, uint32_t *out_size)
{
    uint32_t start = 0, incl_sz, chunk_sz;
    const uint8_t *incl;

    while ((incl = djvu_form_find_chunk(doc, form_off, "INCL", &incl_sz, &start)) != NULL) {
        char id[64];
        uint32_t coff;
        const uint8_t *chunk;
        size_t n = incl_sz < sizeof(id) - 1 ? incl_sz : sizeof(id) - 1;

        memcpy(id, incl, n);
        id[n] = 0;
        djvu_trim_incl_id(id);
        coff = djvu_doc_component_offset(doc, id);
        if (!coff) continue;
        chunk = djvu_form_find_chunk(doc, coff, chunk_id, &chunk_sz, NULL);
        if (chunk) {
            if (out_size) *out_size = chunk_sz;
            return chunk;
        }
    }
    return NULL;
}

const uint8_t *djvu_form_find_chunk(djvu_doc *doc, uint32_t form_off,
                                    const char *id, uint32_t *out_size,
                                    uint32_t *start)
{
    const uint8_t *data = doc->data;
    size_t len = doc->len;
    uint32_t form_size, form_end, pos;

    if ((size_t)form_off + 12 > len || !djvu_tag_eq(data + form_off, "FORM"))
        return NULL;
    form_size = djvu_rd_u32be(data + form_off + 4);
    form_end = form_off + 8 + form_size;
    if (form_end > len) form_end = (uint32_t)len;

    pos = start && *start ? *start : form_off + 12;
    while (pos + 8 <= form_end) {
        const uint8_t *cid = data + pos;
        uint32_t csize = djvu_rd_u32be(data + pos + 4);
        uint32_t cdata = pos + 8;
        uint32_t next;
        if (cdata + csize > form_end) csize = form_end - cdata;
        next = cdata + csize + (csize & 1);
        if (djvu_tag_eq(cid, id)) {
            if (out_size) *out_size = csize;
            if (start) *start = next;
            return data + cdata;
        }
        pos = next;
    }
    return NULL;
}

djvu_doc *djvu_doc_open(djvu_ctx *ctx, const uint8_t *data, size_t len)
{
    djvu_doc *doc;
    uint32_t pos = 0;
    uint32_t form_size, form_end;
    const uint8_t *form_type;

    if (!ctx || !data || len < 16) return NULL;

    /* optional "AT&T" magic */
    if (djvu_tag_eq(data, "AT&T"))
        pos = 4;

    if (pos + 12 > len || !djvu_tag_eq(data + pos, "FORM")) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "not a DjVu file (no FORM)");
        return NULL;
    }
    form_size = djvu_rd_u32be(data + pos + 4);
    form_type = data + pos + 8;
    form_end = pos + 8 + form_size;
    if (form_end > len) form_end = (uint32_t)len;

    doc = (djvu_doc *)djvu_alloc(ctx, sizeof(djvu_doc));
    if (!doc) return NULL;
    memset(doc, 0, sizeof(*doc));
    doc->ctx = ctx;
    doc->data = data;
    doc->len = len;
    doc->root_form_off = pos;

    if (djvu_tag_eq(form_type, "DJVU")) {
        /* single-page document: the top form IS the page */
        doc->pages = (djvu_page_int *)djvu_alloc(ctx, sizeof(djvu_page_int));
        if (!doc->pages) { djvu_free(ctx, doc); return NULL; }
        memset(doc->pages, 0, sizeof(djvu_page_int));
        doc->pages[0].form_off = pos;
        doc->pages[0].form_size = form_size;
        doc->npages = 1;
    } else if (djvu_tag_eq(form_type, "DJVM")) {
        /* multi-page bundle: find DIRM directory */
        uint32_t p = pos + 12;
        int found = 0;
        while (p + 8 <= form_end) {
            const uint8_t *id = data + p;
            uint32_t csize = djvu_rd_u32be(data + p + 4);
            uint32_t cdata = p + 8;
            if (cdata + csize > form_end) csize = form_end - cdata;
            if (djvu_tag_eq(id, "DIRM")) {
                if (load_djvm(doc, cdata, csize) != 0) {
                    djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "bad DIRM directory");
                    djvu_doc_close(doc);
                    return NULL;
                }
                found = 1;
                break;
            }
            p = cdata + csize;
            if (csize & 1) p++;
        }
        if (!found) {
            djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "DJVM without DIRM");
            djvu_doc_close(doc);
            return NULL;
        }
    } else {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "unsupported FORM type");
        djvu_doc_close(doc);
        return NULL;
    }

    djvu_doc_preload_iw44(doc);
    djvu_doc_preload_jb2_dicts(doc);
    return doc;
}

void djvu_doc_close(djvu_doc *doc)
{
    int i;
    if (!doc) return;
    if (doc->comps) {
        for (i = 0; i < doc->ncomp; i++) {
            djvu_free(doc->ctx, doc->comps[i].id);
            djvu_free(doc->ctx, doc->comps[i].title);
        }
        djvu_free(doc->ctx, doc->comps);
    }
    free_jb2_dict_cache(doc->ctx, doc);
    if (doc->pages) {
        for (i = 0; i < doc->npages; i++)
            free_page_iw44(doc->ctx, &doc->pages[i]);
        djvu_free(doc->ctx, doc->pages);
    }
    djvu_free(doc->ctx, doc);
}

int djvu_doc_page_count(djvu_doc *doc)
{
    return doc ? doc->npages : 0;
}

int djvu_doc_page_info(djvu_doc *doc, int page_no, djvu_page_info *info)
{
    djvu_page_int *pg;
    if (!doc || !info || page_no < 0 || page_no >= doc->npages) return -1;
    pg = &doc->pages[page_no];
    if (page_load_info(doc, pg) != 0) return -1;
    *info = pg->info;
    return 0;
}

const char *djvu_doc_page_id(djvu_doc *doc, int page_no)
{
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    return doc->pages[page_no].id;
}

const char *djvu_doc_page_title(djvu_doc *doc, int page_no)
{
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    return doc->pages[page_no].title;
}

int djvu_doc_page_by_name(djvu_doc *doc, const char *name)
{
    int i;
    if (!doc || !name) return -1;
    if (name[0] == '#') name++;
    for (i = 0; i < doc->npages; i++) {
        const char *id = doc->pages[i].id;
        if (id && strcmp(id, name) == 0) return i;
    }
    return -1;
}

djvu_page_type djvu_page_get_type(djvu_doc *doc, int page_no)
{
    uint32_t form_off, sz;
    int has_mask, has_bg, has_fg;
    if (!doc || page_no < 0 || page_no >= doc->npages) return DJVU_PAGE_UNKNOWN;
    form_off = doc->pages[page_no].form_off;
    has_mask = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL) != NULL;
    has_bg   = djvu_form_find_chunk(doc, form_off, "BG44", &sz, NULL) != NULL;
    has_fg   = djvu_form_find_chunk(doc, form_off, "FG44", &sz, NULL) != NULL ||
               djvu_form_find_chunk(doc, form_off, "FGbz", &sz, NULL) != NULL;
    if (has_mask && (has_bg || has_fg)) return DJVU_PAGE_COMPOUND;
    if (has_mask) return DJVU_PAGE_BITONAL;
    if (has_bg || has_fg) return DJVU_PAGE_PHOTO;
    return DJVU_PAGE_UNKNOWN;
}
