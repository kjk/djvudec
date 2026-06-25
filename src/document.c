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

/* Collect all FORM:DJVU components from a DJVM directory (DIRM offsets). */
static int load_djvm(djvu_doc *doc, uint32_t dirm_data, uint32_t dirm_size)
{
    djvu_ctx *ctx = doc->ctx;
    const uint8_t *data = doc->data;
    size_t len = doc->len;
    uint32_t pos = dirm_data;
    uint32_t dirm_end = dirm_data + dirm_size;
    int count, i;
    uint32_t *offs;
    int npages = 0;

    if (pos + 3 > dirm_end || dirm_end > len) return -1;
    /* flag byte (bundled bit + version), then u16 count */
    pos += 1;
    count = (int)djvu_rd_u16be(data + pos);
    pos += 2;
    if (count <= 0 || (size_t)pos + (size_t)count * 4 > len) return -1;

    offs = (uint32_t *)djvu_alloc(ctx, sizeof(uint32_t) * count);
    if (!offs) return -1;
    for (i = 0; i < count; i++)
        offs[i] = djvu_rd_u32be(data + pos + (uint32_t)i * 4);
    /* (the remaining BZZ-compressed name/size section is parsed later) */

    doc->pages = (djvu_page_int *)djvu_alloc(ctx, sizeof(djvu_page_int) * count);
    if (!doc->pages) { djvu_free(ctx, offs); return -1; }
    memset(doc->pages, 0, sizeof(djvu_page_int) * count);

    for (i = 0; i < count; i++) {
        uint32_t o = offs[i];
        if ((size_t)o + 12 > len) continue;
        if (!djvu_tag_eq(data + o, "FORM")) continue;
        if (!djvu_tag_eq(data + o + 8, "DJVU")) continue;  /* only pages */
        doc->pages[npages].form_off = o;
        doc->pages[npages].form_size = djvu_rd_u32be(data + o + 4);
        npages++;
    }
    doc->npages = npages;
    djvu_free(ctx, offs);
    return 0;
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

    return doc;
}

void djvu_doc_close(djvu_doc *doc)
{
    if (!doc) return;
    djvu_free(doc->ctx, doc->pages);
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
