/* djvu_test.c -- CLI harness for the djvu library (jbig2dec-style tool).
 *
 *   djvu_test -info <in.djvu>
 *   djvu_test -page <N> -out <out.pgm|out.ppm> <in.djvu>   (1-based page)
 *   djvu_test -page <N> -text <in.djvu>
 *
 * Used to verify against DjVuLibre's ddjvu / djvutxt. */
#include "djvu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test hook into the internal BZZ decoder */
struct djvu_ctx;
uint8_t *djvu_bzz_decode_all(struct djvu_ctx *ctx, const uint8_t *data,
                             size_t len, size_t *out_len);
void djvu_free(struct djvu_ctx *ctx, void *ptr);
void djvu_debug_dump_comps(djvu_doc *doc);
djvu_image *djvu_debug_render_iw(djvu_doc *doc, int page_no, int kind);
djvu_image *djvu_debug_render_iw_gray(djvu_doc *doc, int page_no, int kind);
djvu_image *djvu_debug_render_iw_plane(djvu_doc *doc, int page_no, int kind, int plane);
djvu_image *djvu_debug_render_bg(djvu_doc *doc, int page_no);
int djvu_debug_dump_iw(djvu_doc *doc, int page_no, int kind, const char *path);

/* timing helpers from bench_ddjvu.cpp (DjVuLibre decode, same clock) */
double bench_now_ms(void);
double bench_ddjvu_page_ms(const char *path, int page0);

static void on_error(void *user, djvu_severity sev, const char *msg)
{
    (void)user;
    const char *s = sev >= DJVU_SEVERITY_ERROR ? "error" :
                    sev == DJVU_SEVERITY_WARNING ? "warning" : "info";
    fprintf(stderr, "djvu %s: %s\n", s, msg);
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static int write_pnm(const char *path, djvu_image *img)
{
    FILE *f = fopen(path, "wb");
    int y;
    if (!f) return -1;
    if (img->format == DJVU_FORMAT_RGB24)
        fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
    else
        fprintf(f, "P5\n%d %d\n255\n", img->width, img->height);
    for (y = 0; y < img->height; y++)
        fwrite(img->data + (size_t)y * img->stride, 1,
               (size_t)img->width * img->format, f);
    fclose(f);
    return 0;
}

static void print_zone(const djvu_text_zone *z, int depth)
{
    static const char *tn[8] = {"?","page","col","region","para","line","word","char"};
    int i;
    int t = (z->type >= 1 && z->type <= 7) ? z->type : 0;
    for (i = 0; i < depth; i++) fputs("  ", stdout);
    printf("%s [%d,%d %dx%d]", tn[t], z->x, z->y, z->w, z->h);
    if (z->type == DJVU_ZONE_WORD && z->text) printf(" \"%s\"", z->text);
    putchar('\n');
    for (i = 0; i < z->nchildren; i++) print_zone(&z->children[i], depth + 1);
}

static void print_outline(const djvu_outline_item *it, int depth)
{
    int i;
    if (it->title) {
        for (i = 0; i < depth; i++) fputs("  ", stdout);
        printf("%s -> %s (page %d)\n", it->title,
               it->url ? it->url : "", it->page_no);
    }
    for (i = 0; i < it->nchildren; i++) print_outline(&it->children[i], depth + 1);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int do_info = 0, do_text = 0, do_bzz = 0, do_iw = 0, page = 1;
    int do_zones = 0, do_outline = 0, do_links = 0, do_type = 0, do_bench = 0;
    int i, rc = 0;
    uint8_t *data; size_t len;
    djvu_ctx *ctx; djvu_doc *doc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-info")) do_info = 1;
        else if (!strcmp(argv[i], "-text")) do_text = 1;
        else if (!strcmp(argv[i], "-bzzdec")) do_bzz = 1;
        else if (!strcmp(argv[i], "-comps")) do_info = 2;
        else if (!strcmp(argv[i], "-bg")) do_iw = 8;         /* full-page background */
        else if (!strcmp(argv[i], "-iwbggray")) do_iw = 5;   /* BG44 Y plane */
        else if (!strcmp(argv[i], "-iwbgcb")) do_iw = 6;     /* BG44 Cb plane */
        else if (!strcmp(argv[i], "-iwbgcr")) do_iw = 7;     /* BG44 Cr plane */
        else if (!strcmp(argv[i], "-iwbg")) do_iw = 1;       /* render BG44 (mine) */
        else if (!strcmp(argv[i], "-iwfg")) do_iw = 2;       /* render FG44 (mine) */
        else if (!strcmp(argv[i], "-iwdumpbg")) do_iw = 3;   /* dump BG44 as PM44 */
        else if (!strcmp(argv[i], "-iwdumpfg")) do_iw = 4;   /* dump FG44 as PM44 */
        else if (!strcmp(argv[i], "-zones")) do_zones = 1;   /* text zone tree */
        else if (!strcmp(argv[i], "-outline")) do_outline = 1;
        else if (!strcmp(argv[i], "-links")) do_links = 1;
        else if (!strcmp(argv[i], "-type")) do_type = 1;
        else if (!strcmp(argv[i], "-bench")) do_bench = 1;
        else if (!strcmp(argv[i], "-page") && i + 1 < argc) page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-out") && i + 1 < argc) out = argv[++i];
        else in = argv[i];
    }
    if (!in) {
        fprintf(stderr, "usage: djvu_test [-info] [-page N] [-out f.pnm] [-text] in.djvu\n");
        return 2;
    }

    data = read_file(in, &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    ctx = djvu_ctx_new(NULL, NULL, on_error, NULL);

    if (do_bzz) {
        /* decode a raw BZZ stream and write to -out (or stdout) */
        size_t olen = 0;
        uint8_t *o = djvu_bzz_decode_all(ctx, data, len, &olen);
        if (!o) { rc = 1; }
        else {
            FILE *f = out ? fopen(out, "wb") : stdout;
            if (f) { fwrite(o, 1, olen, f); if (out) fclose(f); }
            djvu_free(ctx, o);
        }
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }
    doc = djvu_doc_open(ctx, data, len);
    if (!doc) { fprintf(stderr, "cannot open document\n"); free(data); djvu_ctx_free(ctx); return 1; }

    if (do_iw) {
        if (do_iw == 3 || do_iw == 4) {
            rc = djvu_debug_dump_iw(doc, page - 1, do_iw == 4, out ? out : "iw.djvu");
        } else if (do_iw == 8) {
            djvu_image *img = djvu_debug_render_bg(doc, page - 1);
            if (img) { if (out) write_pnm(out, img); djvu_image_destroy(ctx, img); }
            else rc = 1;
        } else if (do_iw == 5 || do_iw == 6 || do_iw == 7) {
            djvu_image *img = djvu_debug_render_iw_plane(doc, page - 1, 0, do_iw - 5);
            if (img) { if (out) write_pnm(out, img); djvu_image_destroy(ctx, img); }
            else rc = 1;
        } else {
            djvu_image *img = djvu_debug_render_iw(doc, page - 1, do_iw == 2);
            if (img) {
                if (out) write_pnm(out, img);
                djvu_image_destroy(ctx, img);
            } else rc = 1;
        }
        djvu_doc_close(doc); djvu_ctx_free(ctx); free(data);
        return rc;
    }

    if (do_bench) {
        int n = djvu_doc_page_count(doc);
        for (i = 0; i < n; i++) {
            double t0 = bench_now_ms();
            djvu_image *img = djvu_page_render(doc, i, 1);
            double mine = bench_now_ms() - t0;
            if (img) djvu_image_destroy(ctx, img);
            double lib = bench_ddjvu_page_ms(in, i);
            if (lib < 0 || !img) {
                printf("page %d, djvulibre %s, ours %.2f ms%s\n", i + 1,
                       lib < 0 ? "ERROR" : "ok", mine, img ? "" : " (ours FAILED)");
                continue;
            }
            double diff = mine - lib;            /* + => ours slower */
            double pct = lib > 0 ? diff / lib * 100.0 : 0.0;
            printf("page %d, djvulibre %.2f ms, ours %.2f ms, %+.2f ms, %+.1f%%\n",
                   i + 1, lib, mine, diff, pct);
        }
        djvu_doc_close(doc); djvu_ctx_free(ctx); free(data);
        return rc;
    }

    if (do_info == 2) djvu_debug_dump_comps(doc);

    if (do_info || (!out && !do_text && !do_zones && !do_outline && !do_links && !do_type)) {
        int n = djvu_doc_page_count(doc);
        printf("pages: %d\n", n);
        for (i = 0; i < n; i++) {
            djvu_page_info pi;
            if (djvu_doc_page_info(doc, i, &pi) == 0)
                printf("page %d: %dx%d dpi=%d ver=%d rot=%d\n",
                       i + 1, pi.width, pi.height, pi.dpi, pi.version, pi.rotation);
            else
                printf("page %d: <no info>\n", i + 1);
        }
    }

    if (do_text) {
        char *t = djvu_page_text(doc, page - 1);
        if (t) { fputs(t, stdout); djvu_text_destroy(ctx, t); }
    }

    if (do_type) {
        static const char *tn[4] = {"unknown","bitonal","photo","compound"};
        djvu_page_type t = djvu_page_get_type(doc, page - 1);
        printf("page %d type: %s\n", page, tn[(t >= 0 && t < 4) ? t : 0]);
    }

    if (do_zones) {
        djvu_page_text_zones *z = djvu_page_text_get_zones(doc, page - 1);
        if (z && z->root) print_zone(z->root, 0);
        else printf("(no text zones)\n");
        djvu_text_zones_destroy(ctx, z);
    }

    if (do_outline) {
        djvu_outline_item *root = djvu_doc_outline(doc);
        if (root) print_outline(root, -1);
        else printf("(no outline)\n");
        djvu_outline_destroy(ctx, root);
    }

    if (do_links) {
        djvu_page_links *L = djvu_page_get_links(doc, page - 1);
        if (L) {
            int k;
            for (k = 0; k < L->nlinks; k++) {
                djvu_link *e = &L->links[k];
                printf("link %d: [%d,%d %dx%d] shape=%d -> %s%s%s\n", k,
                       e->x, e->y, e->w, e->h, e->shape, e->url,
                       e->comment ? " ; " : "", e->comment ? e->comment : "");
            }
            djvu_page_links_destroy(ctx, L);
        } else printf("(no links)\n");
    }

    if (out) {
        djvu_image *img = djvu_page_render(doc, page - 1, 1);
        if (img) {
            if (write_pnm(out, img) != 0) { fprintf(stderr, "cannot write %s\n", out); rc = 1; }
            djvu_image_destroy(ctx, img);
        } else { rc = 1; }
    }

    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    free(data);
    return rc;
}
