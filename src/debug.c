/* debug.c -- test-harness helpers (IW44 dumps, per-layer renders, etc.). */
#include "djvu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void djvu_debug_dump_comps(djvu_doc *doc)
{
    int i;
    const char *tn[4] = {"incl", "page", "thumb", "anno"};
    if (!doc) return;
    printf("components: %d\n", doc->ncomp);
    for (i = 0; i < doc->ncomp; i++) {
        djvu_component *c = &doc->comps[i];
        printf("  [%d] off=%u size=%u type=%s id=%s\n", i, c->offset, c->size,
               (c->type >= 0 && c->type < 4) ? tn[c->type] : "?",
               c->id ? c->id : "(null)");
    }
}

static iw_pixmap *debug_decode_iw_form(djvu_doc *doc, uint32_t form_off, const char *id)
{
    iw_pixmap *pm = djvu_iw44_new(doc->ctx);
    const char *mc;
    int maxc;

    if (!pm) return NULL;
    mc = getenv("DJVU_IW_MAXCHUNKS");
    maxc = mc ? atoi(mc) : 1000;
    if (djvu_iw44_decode_form(doc, form_off, id, pm, maxc) != 0) {
        djvu_iw44_free(pm);
        return NULL;
    }
    return pm;
}

djvu_image *djvu_debug_render_iw(djvu_doc *doc, int page_no, int kind)
{
    djvu_ctx *ctx;
    uint32_t form_off;
    const char *id = kind ? "FG44" : "BG44";
    iw_pixmap *pm;
    djvu_image *out = NULL;
    int w, h;

    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;
    pm = debug_decode_iw_form(doc, form_off, id);
    if (!pm) return NULL;
    w = djvu_iw44_width(pm); h = djvu_iw44_height(pm);
    if (w <= 0 || h <= 0) { djvu_iw44_free(pm); return NULL; }
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_iw44_free(pm); return NULL; }
    out->width = w; out->height = h; out->format = DJVU_FORMAT_RGB24;
    out->stride = w * 3;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)w * h * 3);
    if (!out->data || djvu_iw44_render_rgb(pm, out->data) != 0) {
        djvu_image_destroy(ctx, out); djvu_iw44_free(pm); return NULL;
    }
    djvu_iw44_free(pm);
    return out;
}

djvu_image *djvu_debug_render_iw_gray(djvu_doc *doc, int page_no, int kind)
{
    djvu_ctx *ctx;
    uint32_t form_off;
    const char *id = kind ? "FG44" : "BG44";
    iw_pixmap *pm;
    djvu_image *out = NULL;
    int w, h;
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    form_off = doc->pages[page_no].form_off;
    pm = debug_decode_iw_form(doc, form_off, id);
    if (!pm) return NULL;
    w = djvu_iw44_width(pm); h = djvu_iw44_height(pm);
    if (w <= 0 || h <= 0) { djvu_iw44_free(pm); return NULL; }
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_iw44_free(pm); return NULL; }
    out->width = w; out->height = h; out->format = DJVU_FORMAT_GRAY8; out->stride = w;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)w * h);
    if (!out->data || djvu_iw44_render_gray(pm, out->data) != 0) {
        djvu_image_destroy(ctx, out); djvu_iw44_free(pm); return NULL;
    }
    djvu_iw44_free(pm);
    return out;
}

djvu_image *djvu_debug_render_iw_plane(djvu_doc *doc, int page_no, int kind, int plane)
{
    djvu_ctx *ctx; uint32_t form_off;
    const char *id = kind ? "FG44" : "BG44"; iw_pixmap *pm; djvu_image *out;
    int w, h;
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx; form_off = doc->pages[page_no].form_off;
    pm = debug_decode_iw_form(doc, form_off, id);
    if (!pm) return NULL;
    w = djvu_iw44_width(pm); h = djvu_iw44_height(pm);
    if (w <= 0 || h <= 0) { djvu_iw44_free(pm); return NULL; }
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_iw44_free(pm); return NULL; }
    out->width = w; out->height = h; out->format = DJVU_FORMAT_GRAY8; out->stride = w;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)w * h);
    if (!out->data || djvu_iw44_render_plane(pm, plane, out->data) != 0) {
        djvu_image_destroy(ctx, out); djvu_iw44_free(pm); return NULL;
    }
    djvu_iw44_free(pm);
    return out;
}

djvu_image *djvu_debug_render_bg(djvu_doc *doc, int page_no)
{
    djvu_ctx *ctx; djvu_page_info info; djvu_cpix bg; djvu_image *out;
    if (!doc || page_no < 0 || page_no >= doc->npages) return NULL;
    ctx = doc->ctx;
    if (djvu_doc_page_info(doc, page_no, &info) != 0) return NULL;
    memset(&bg, 0, sizeof(bg));
    if (djvu_compose_background(doc, doc->pages[page_no].form_off,
                               info.width, info.height, &bg) != 0) return NULL;
    out = (djvu_image *)djvu_alloc(ctx, sizeof(djvu_image));
    if (!out) { djvu_cpix_free(ctx, &bg); return NULL; }
    out->width = bg.w; out->height = bg.h; out->format = DJVU_FORMAT_RGB24;
    out->stride = bg.w * 3;
    out->data = (uint8_t *)djvu_alloc(ctx, (size_t)bg.w * bg.h * 3);
    if (!out->data) { djvu_free(ctx, out); djvu_cpix_free(ctx, &bg); return NULL; }
    djvu_flip_rgb_bottomup(out->data, bg.d, bg.w, bg.h);
    djvu_cpix_free(ctx, &bg);
    return out;
}

static void put_u32be(FILE *f, uint32_t v)
{
    fputc((v >> 24) & 0xff, f); fputc((v >> 16) & 0xff, f);
    fputc((v >> 8) & 0xff, f); fputc(v & 0xff, f);
}

int djvu_debug_dump_iw(djvu_doc *doc, int page_no, int kind, const char *path)
{
    uint32_t form_off, start = 0, sz;
    const uint8_t *chunk;
    const char *id = kind ? "FG44" : "BG44";
    FILE *f;
    uint32_t total = 4;
    const uint8_t *chunks[64]; uint32_t sizes[64]; int n = 0, i;

    if (!doc || page_no < 0 || page_no >= doc->npages) return -1;
    form_off = doc->pages[page_no].form_off;
    {
        const char *mc = getenv("DJVU_IW_MAXCHUNKS");
        int maxc = mc ? atoi(mc) : 1000;
        while ((chunk = djvu_form_find_chunk(doc, form_off, id, &sz, &start)) != NULL && n < 64) {
            if (n >= maxc) break;
            chunks[n] = chunk; sizes[n] = sz; n++;
            total += 8 + sz + (sz & 1);
        }
    }
    if (n == 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("AT&TFORM", 1, 8, f);
    put_u32be(f, total);
    fwrite("PM44", 1, 4, f);
    for (i = 0; i < n; i++) {
        fwrite("PM44", 1, 4, f);
        put_u32be(f, sizes[i]);
        fwrite(chunks[i], 1, sizes[i], f);
        if (sizes[i] & 1) fputc(0, f);
    }
    fclose(f);
    return 0;
}