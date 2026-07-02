/* size_probe_djvudec.c -- minimal djvudec driver for code-size comparison.
 *
 * Exercises decode-path APIs used by viewers: page render, hidden text zones,
 * hyperlinks, and document outline. Linked with production src (no debug.c). */
#include "djvu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile unsigned long g_sink;

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static void touch_image(const djvu_image *img)
{
    int y, x, stride, bpp;

    if (!img || !img->data)
        return;
    stride = img->stride;
    bpp = (int)img->format;
    for (y = 0; y < img->height; y += 17) {
        for (x = 0; x < img->width; x += 13)
            g_sink += img->data[(size_t)y * (size_t)stride + (size_t)x * (size_t)bpp];
    }
}

static void touch_zone(const djvu_text_zone *z)
{
    int i;

    if (!z)
        return;
    g_sink += (unsigned long)z->type + (unsigned long)z->x + (unsigned long)z->y;
    if (z->text && z->text[0])
        g_sink += (unsigned char)z->text[0];
    for (i = 0; i < z->nchildren; i++)
        touch_zone(&z->children[i]);
}

static void touch_outline(djvu_outline_item *item)
{
    int i;

    if (!item)
        return;
    if (item->title && item->title[0])
        g_sink += (unsigned char)item->title[0];
    if (item->url && item->url[0])
        g_sink += (unsigned char)item->url[0];
    g_sink += (unsigned long)item->page_no;
    for (i = 0; i < item->nchildren; i++)
        touch_outline(&item->children[i]);
}

static void touch_links(const djvu_page_links *links)
{
    int i;

    if (!links)
        return;
    for (i = 0; i < links->nlinks; i++)
        g_sink += (unsigned long)links->links[i].shape + (unsigned long)links->links[i].x;
}

int main(int argc, char **argv)
{
    const char *path;
    uint8_t *data;
    size_t len;
    djvu_ctx *ctx;
    djvu_doc *doc;
    djvu_outline_item *outline;
    int np, p;

    if (argc < 2) {
        fprintf(stderr, "usage: %s file.djvu\n", argv[0]);
        return 2;
    }
    path = argv[1];

    data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    djvu_init();
    ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, NULL, NULL);
    if (!ctx) {
        free(data);
        return 1;
    }

    doc = djvu_doc_open(ctx, data, len);
    if (!doc) {
        djvu_ctx_free(ctx);
        free(data);
        return 1;
    }

    np = djvu_doc_page_count(doc);
    g_sink += (unsigned long)np;

    outline = djvu_doc_outline(doc);
    touch_outline(outline);
    djvu_outline_destroy(ctx, outline);

    for (p = 0; p < np; p++) {
        djvu_page_info info;
        djvu_image *img;
        djvu_page_text_zones *zones;
        djvu_page_links *links;

        if (djvu_doc_page_info(doc, p, &info) == 0)
            g_sink += (unsigned long)info.width + (unsigned long)info.height;

        g_sink += (unsigned long)djvu_page_get_type(doc, p);

        img = djvu_page_render(doc, p, 1);
        touch_image(img);
        djvu_image_destroy(ctx, img);

        zones = djvu_page_text_get_zones(doc, p);
        if (zones) {
            if (zones->text && zones->text[0])
                g_sink += (unsigned char)zones->text[0];
            touch_zone(zones->root);
            djvu_text_zones_destroy(ctx, zones);
        }

        links = djvu_page_get_links(doc, p);
        touch_links(links);
        djvu_page_links_destroy(ctx, links);
    }

    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    free(data);

    printf("%lu\n", g_sink);
    return 0;
}