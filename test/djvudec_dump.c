/* djvudec_dump.c -- inspect .djvu files using the djvudec library only.
 *
 * Public API (djvu.h) examples: -info, -text, -zones, -outline, -links, -type,
 * -id, -title, -resolve, -out (render). Codec/layer introspection uses
 * djvu_internal.h helpers (-comps, -dump-features, -bzzdec, -iw*, -bg).
 *
 *   djvudec_dump -info file.djvu
 *   djvudec_dump -page 3 -zones -text file.djvu
 *   djvudec_dump -dump-features file.djvu
 *
 * No DjVuLibre dependency; links only src/*.c + this file. */
#include "djvu.h"
#include "djvu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

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

static double now_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static int write_pnm(const char *path, djvu_image *img)
{
    FILE *f;
    int y;
    if (path && !strcmp(path, "-")) {
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        f = stdout;
    } else {
        f = fopen(path, "wb");
        if (!f) return -1;
    }
    if (img->format == DJVU_FORMAT_RGB24)
        fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
    else
        fprintf(f, "P5\n%d %d\n255\n", img->width, img->height);
    for (y = 0; y < img->height; y++)
        fwrite(img->data + (size_t)y * (size_t)img->stride, 1,
               (size_t)img->width * img->format, f);
    if (f != stdout) fclose(f);
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

static const char *page_type_name(djvu_page_type t)
{
    static const char *names[] = {"unknown", "bitonal", "photo", "compound"};
    return names[(t >= 0 && t <= 3) ? t : 0];
}

typedef enum { PK_OTHER, PK_MASK, PK_BG } page_kind_t;

static page_kind_t page_kind(djvu_doc *doc, int page0)
{
    uint32_t form_off = doc->pages[page0].form_off;
    uint32_t sz;
    int has_sjbz = djvu_form_find_chunk(doc, form_off, "Sjbz", &sz, NULL) != NULL;
    int has_bg = djvu_form_find_chunk(doc, form_off, "BG44", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "FG44", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "BGjp", &sz, NULL) != NULL ||
                 djvu_form_find_chunk(doc, form_off, "FGjp", &sz, NULL) != NULL;
    if (has_bg) return PK_BG;
    if (has_sjbz) return PK_MASK;
    return PK_OTHER;
}

static const char *page_kind_name(page_kind_t k)
{
    if (k == PK_MASK) return "mask";
    if (k == PK_BG) return "bg";
    return "other";
}

static int page_has_chunk(djvu_doc *doc, int page0, const char *cid)
{
    uint32_t sz;
    return djvu_form_find_chunk(doc, doc->pages[page0].form_off, cid, &sz, NULL) != NULL;
}

static int page_has_incl(djvu_doc *doc, int page0)
{
    uint32_t sz, start = 0;
    return djvu_form_find_chunk(doc, doc->pages[page0].form_off, "INCL", &sz, &start) != NULL;
}

static const char *container_kind(djvu_doc *doc)
{
    const uint8_t *data = doc->data;
    size_t len = doc->len;
    uint32_t pos = doc->root_form_off;

    if (pos + 12 > len) return "unknown";
    if (djvu_tag_eq(data + pos + 8, "DJVU")) return "single";
    if (!djvu_tag_eq(data + pos + 8, "DJVM")) return "unknown";
    {
        uint32_t form_end = pos + 8 + djvu_rd_u32be(data + pos + 4);
        uint32_t p = pos + 12;
        if (form_end > len) form_end = (uint32_t)len;
        while (p + 8 <= form_end) {
            uint32_t csz = djvu_rd_u32be(data + p + 4);
            uint32_t chunk_end = p + 8 + csz;
            if (chunk_end > form_end) break;
            if (djvu_tag_eq(data + p, "DIRM") && p + 11 <= chunk_end)
                return ((data[p + 8] >> 7) & 1) ? "bundled" : "indirect";
            p = chunk_end + (csz & 1);
        }
    }
    return "djvm";
}

static void run_info(djvu_doc *doc)
{
    int n = djvu_doc_page_count(doc);
    int i;
    printf("pages: %d\n", n);
    for (i = 0; i < n; i++) {
        djvu_page_info pi;
        const char *id = djvu_doc_page_id(doc, i);
        const char *title = djvu_doc_page_title(doc, i);
        if (djvu_doc_page_info(doc, i, &pi) == 0)
            printf("page %d: %dx%d dpi=%d ver=%d rot=%d id=%s title=%s\n",
                   i + 1, pi.width, pi.height, pi.dpi, pi.version, pi.rotation,
                   id ? id : "", title ? title : "");
        else
            printf("page %d: <no info> id=%s title=%s\n",
                   i + 1, id ? id : "", title ? title : "");
    }
}

static int run_dump_features(djvu_doc *doc)
{
    djvu_ctx *ctx = doc->ctx;
    int npages = djvu_doc_page_count(doc);
    int i, ncomp_djvi = 0;
    int file_incl = 0, file_incl_djbz = 0, file_inline_djbz = 0;
    int file_outline = 0, file_text = 0, file_annot = 0, file_links = 0;
    int file_fgbz = 0, file_fg44 = 0, file_bgjp = 0, file_fgjp = 0;
    int type_bitonal = 0, type_photo = 0, type_compound = 0;
    int kind_mask = 0, kind_bg = 0, kind_other = 0;
    int rot_nonzero = 0;
    double total_ms = 0.0;
    djvu_outline_item *outline;

    if (doc->comps) {
        for (i = 0; i < doc->ncomp; i++)
            if (doc->comps[i].type == 0) ncomp_djvi++;
    }
    outline = djvu_doc_outline(doc);
    file_outline = (outline && outline->nchildren > 0) ? 1 : 0;
    if (outline) djvu_outline_destroy(ctx, outline);

    for (i = 0; i < npages; i++) {
        djvu_page_type pt = djvu_page_get_type(doc, i);
        page_kind_t pk = page_kind(doc, i);
        djvu_page_info pi;
        int has_text = 0, has_annot = 0, has_links = 0;
        int incl = page_has_incl(doc, i);
        int inline_djbz = page_has_chunk(doc, i, "Djbz");
        int incl_djbz = djvu_form_find_incl_chunk(doc, doc->pages[i].form_off,
                                                  "Djbz", NULL) != NULL;
        double t0, dt;
        djvu_image *img;
        djvu_page_links *L;

        if (pt == DJVU_PAGE_BITONAL) type_bitonal = 1;
        else if (pt == DJVU_PAGE_PHOTO) type_photo = 1;
        else if (pt == DJVU_PAGE_COMPOUND) type_compound = 1;
        if (pk == PK_MASK) kind_mask = 1;
        else if (pk == PK_BG) kind_bg = 1;
        else kind_other = 1;

        if (page_has_chunk(doc, i, "FGbz")) file_fgbz = 1;
        if (page_has_chunk(doc, i, "FG44")) file_fg44 = 1;
        if (page_has_chunk(doc, i, "BGjp")) file_bgjp = 1;
        if (page_has_chunk(doc, i, "FGjp")) file_fgjp = 1;
        if (incl) file_incl = 1;
        if (incl_djbz) file_incl_djbz = 1;
        if (inline_djbz) file_inline_djbz = 1;

        {
            char *tx = djvu_page_text(doc, i);
            has_text = (tx && tx[0]) ? 1 : 0;
            if (tx) djvu_text_destroy(ctx, tx);
        }
        if (page_has_chunk(doc, i, "ANTa") || page_has_chunk(doc, i, "ANTz"))
            has_annot = 1;
        L = djvu_page_get_links(doc, i);
        if (L) {
            has_links = (L->nlinks > 0) ? 1 : 0;
            djvu_page_links_destroy(ctx, L);
        }
        if (has_text) file_text = 1;
        if (has_annot) file_annot = 1;
        if (has_links) file_links = 1;

        pi.rotation = 0;
        if (djvu_doc_page_info(doc, i, &pi) == 0 && pi.rotation != 0)
            rot_nonzero = 1;

        t0 = now_ms();
        img = djvu_page_render(doc, i, 1);
        dt = now_ms() - t0;
        if (img) djvu_image_destroy(ctx, img);
        total_ms += dt;

        printf("page\t%d\ttype\t%s\tkind\t%s\trot\t%d\ttext\t%d\tannot\t%d\t"
               "links\t%d\tincl\t%d\tinline_djbz\t%d\tincl_djbz\t%d\t"
               "fgbz\t%d\tfg44\t%d\tbgjp\t%d\tfgjp\t%d\trender_ms\t%.3f\n",
               i + 1, page_type_name(pt), page_kind_name(pk),
               (pi.rotation == 90 || pi.rotation == 180 || pi.rotation == 270)
                   ? pi.rotation : 0,
               has_text, has_annot, has_links, incl, inline_djbz, incl_djbz,
               page_has_chunk(doc, i, "FGbz"), page_has_chunk(doc, i, "FG44"),
               page_has_chunk(doc, i, "BGjp"), page_has_chunk(doc, i, "FGjp"),
               dt);
    }

    printf("summary\tpages\t%d\tcontainer\t%s\tncomp_djvi\t%d\t"
           "incl\t%d\tincl_djbz\t%d\tinline_djbz\t%d\toutline\t%d\t"
           "text\t%d\tannot\t%d\tlinks\t%d\t"
           "type_bitonal\t%d\ttype_photo\t%d\ttype_compound\t%d\t"
           "kind_mask\t%d\tkind_bg\t%d\tkind_other\t%d\t"
           "fgbz\t%d\tfg44\t%d\tbgjp\t%d\tfgjp\t%d\trot_nonzero\t%d\t"
           "total_render_ms\t%.3f\n",
           npages, container_kind(doc), ncomp_djvi,
           file_incl, file_incl_djbz, file_inline_djbz, file_outline,
           file_text, file_annot, file_links,
           type_bitonal, type_photo, type_compound,
           kind_mask, kind_bg, kind_other,
           file_fgbz, file_fg44, file_bgjp, file_fgjp, rot_nonzero,
           total_ms);
    return 0;
}

typedef struct {
    int do_help;
    int do_info;
    int do_comps;
    int do_text;
    int do_zones;
    int do_outline;
    int do_links;
    int do_type;
    int do_id;
    int do_title;
    int do_dump_features;
    int do_bzz;
    int do_iw;
    int do_all;
    int page;
    const char *out;
    const char *resolve;
    const char *in;
} opts_t;

static void usage(void)
{
    fputs(
        "djvudec_dump -- inspect DjVu files (djvudec library only)\n"
        "\n"
        "  djvudec_dump [options] file.djvu\n"
        "\n"
        "Document / page (public API):\n"
        "  -info              page dimensions, dpi, rotation, id, title\n"
        "  -page N            page number, 1-based (default 1)\n"
        "  -all               apply page flags to every page\n"
        "  -text              hidden page text (UTF-8)\n"
        "  -zones             text zone tree with bounding boxes\n"
        "  -outline           document bookmark outline\n"
        "  -links             page hyperlinks / mapareas\n"
        "  -type              page content type (bitonal/photo/compound)\n"
        "  -id                page component id from directory\n"
        "  -title             page title from directory\n"
        "  -resolve NAME      resolve named destination to page number\n"
        "  -out FILE          render page composite to PGM/PPM (- = stdout)\n"
        "\n"
        "Corpus / structure (internal introspection):\n"
        "  -comps             list DJVM components (incl/page/thumb/anno)\n"
        "  -dump-features     tab-separated feature + render-time dump\n"
        "\n"
        "Codec layers (no full composite):\n"
        "  -bzzdec -out FILE   decode raw BZZ stream (no document open)\n"
        "  -bg                composite background layer only (RGB)\n"
        "  -iwbg              render BG44 IW44 layer (RGB)\n"
        "  -iwfg              render FG44 IW44 layer (RGB)\n"
        "  -iwbggray          BG44 luma plane (gray PGM)\n"
        "  -iwbgcb            BG44 Cb plane (gray PGM)\n"
        "  -iwbgcr            BG44 Cr plane (gray PGM)\n"
        "  -iwdumpbg FILE     dump BG44 chunks as FORM:PM44\n"
        "  -iwdumpfg FILE     dump FG44 chunks as FORM:PM44\n"
        "\n"
        "With no flags, -info is implied.\n",
        stderr);
}

static int parse_args(int argc, char **argv, opts_t *o)
{
    int i;
    memset(o, 0, sizeof(*o));
    o->page = 1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help") ||
            !strcmp(argv[i], "-h"))
            o->do_help = 1;
        else if (!strcmp(argv[i], "-info")) o->do_info = 1;
        else if (!strcmp(argv[i], "-comps")) o->do_comps = 1;
        else if (!strcmp(argv[i], "-text")) o->do_text = 1;
        else if (!strcmp(argv[i], "-zones")) o->do_zones = 1;
        else if (!strcmp(argv[i], "-outline")) o->do_outline = 1;
        else if (!strcmp(argv[i], "-links")) o->do_links = 1;
        else if (!strcmp(argv[i], "-type")) o->do_type = 1;
        else if (!strcmp(argv[i], "-id")) o->do_id = 1;
        else if (!strcmp(argv[i], "-title")) o->do_title = 1;
        else if (!strcmp(argv[i], "-dump-features")) o->do_dump_features = 1;
        else if (!strcmp(argv[i], "-bzzdec")) o->do_bzz = 1;
        else if (!strcmp(argv[i], "-all")) o->do_all = 1;
        else if (!strcmp(argv[i], "-bg")) o->do_iw = 8;
        else if (!strcmp(argv[i], "-iwbggray")) o->do_iw = 5;
        else if (!strcmp(argv[i], "-iwbgcb")) o->do_iw = 6;
        else if (!strcmp(argv[i], "-iwbgcr")) o->do_iw = 7;
        else if (!strcmp(argv[i], "-iwbg")) o->do_iw = 1;
        else if (!strcmp(argv[i], "-iwfg")) o->do_iw = 2;
        else if (!strcmp(argv[i], "-iwdumpbg") && i + 1 < argc) {
            o->do_iw = 3;
            o->out = argv[++i];
        } else if (!strcmp(argv[i], "-iwdumpfg") && i + 1 < argc) {
            o->do_iw = 4;
            o->out = argv[++i];
        } else if (!strcmp(argv[i], "-page") && i + 1 < argc)
            o->page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-out") && i + 1 < argc)
            o->out = argv[++i];
        else if (!strcmp(argv[i], "-resolve") && i + 1 < argc)
            o->resolve = argv[++i];
        else if (argv[i][0] == '-')
            return -1;
        else
            o->in = argv[i];
    }
    return 0;
}

static int run_page_ops(djvu_doc *doc, djvu_ctx *ctx, const opts_t *o, int page0)
{
    int rc = 0;

    if (o->do_text) {
        char *t = djvu_page_text(doc, page0);
        if (o->do_all) printf("--- page %d text ---\n", page0 + 1);
        if (t) { fputs(t, stdout); if (!o->do_all) fputs("\n", stdout); djvu_text_destroy(ctx, t); }
        else if (!o->do_all) printf("(no text)\n");
    }

    if (o->do_type) {
        djvu_page_type t = djvu_page_get_type(doc, page0);
        printf("page %d type: %s\n", page0 + 1, page_type_name(t));
    }

    if (o->do_id) {
        const char *id = djvu_doc_page_id(doc, page0);
        printf("page %d id: %s\n", page0 + 1, id ? id : "(none)");
    }

    if (o->do_title) {
        const char *title = djvu_doc_page_title(doc, page0);
        printf("page %d title: %s\n", page0 + 1, title ? title : "(none)");
    }

    if (o->do_zones) {
        djvu_page_text_zones *z = djvu_page_text_get_zones(doc, page0);
        if (o->do_all) printf("--- page %d zones ---\n", page0 + 1);
        if (z && z->root) print_zone(z->root, 0);
        else printf("(no text zones)\n");
        djvu_text_zones_destroy(ctx, z);
    }

    if (o->do_links) {
        djvu_page_links *L = djvu_page_get_links(doc, page0);
        if (o->do_all) printf("--- page %d links ---\n", page0 + 1);
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

    if (o->do_iw) {
        djvu_image *img = NULL;
        if (o->do_iw == 3 || o->do_iw == 4) {
            rc = djvu_debug_dump_iw(doc, page0, o->do_iw == 4,
                                    o->out ? o->out : "iw.djvu");
        } else if (o->do_iw == 8) {
            img = djvu_debug_render_bg(doc, page0);
        } else if (o->do_iw == 5 || o->do_iw == 6 || o->do_iw == 7) {
            img = djvu_debug_render_iw_plane(doc, page0, 0, o->do_iw - 5);
        } else {
            img = djvu_debug_render_iw(doc, page0, o->do_iw == 2);
        }
        if (o->do_iw != 3 && o->do_iw != 4) {
            if (img) {
                if (o->out) {
                    if (write_pnm(o->out, img) != 0) rc = 1;
                } else {
                    fprintf(stderr, "-out required for layer render\n");
                    rc = 1;
                }
                djvu_image_destroy(ctx, img);
            } else rc = 1;
        }
    }

    if (o->out && !o->do_bzz && !o->do_iw) {
        djvu_image *img = djvu_page_render(doc, page0, 1);
        if (img) {
            if (write_pnm(o->out, img) != 0) {
                fprintf(stderr, "cannot write %s\n", o->out);
                rc = 1;
            }
            djvu_image_destroy(ctx, img);
        } else rc = 1;
    }

    return rc;
}

int main(int argc, char **argv)
{
    opts_t o;
    uint8_t *data = NULL;
    size_t len = 0;
    djvu_ctx *ctx = NULL;
    djvu_doc *doc = NULL;
    int rc = 0;
    int i;

    if (parse_args(argc, argv, &o) != 0 || o.do_help) {
        usage();
        return o.do_help ? 0 : 2;
    }
    if (!o.in) {
        usage();
        return 2;
    }

    data = read_file(o.in, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", o.in);
        return 1;
    }

    ctx = djvu_ctx_new(NULL, NULL, on_error, NULL);
    if (!ctx) { free(data); return 1; }

    if (o.do_bzz) {
        size_t olen = 0;
        uint8_t *out = djvu_bzz_decode_all(ctx, data, len, &olen);
        if (!out) rc = 1;
        else {
            FILE *f = o.out ? fopen(o.out, "wb") : stdout;
            if (f) {
                fwrite(out, 1, olen, f);
                if (o.out) fclose(f);
            } else rc = 1;
            djvu_free(ctx, out);
        }
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

    doc = djvu_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "cannot open document\n");
        free(data);
        djvu_ctx_free(ctx);
        return 1;
    }

    if (o.do_dump_features) {
        rc = run_dump_features(doc);
        goto done;
    }

    if (o.resolve) {
        int p = djvu_doc_page_by_name(doc, o.resolve);
        printf("%s -> page %d\n", o.resolve, p >= 0 ? p + 1 : -1);
    }

    if (o.do_comps) djvu_debug_dump_comps(doc);

    if (o.do_info || (!o.do_text && !o.do_zones && !o.do_outline && !o.do_links &&
                      !o.do_type && !o.do_id && !o.do_title && !o.out && !o.do_iw &&
                      !o.resolve))
        run_info(doc);

    if (o.do_outline) {
        djvu_outline_item *root = djvu_doc_outline(doc);
        if (root) print_outline(root, -1);
        else printf("(no outline)\n");
        djvu_outline_destroy(ctx, root);
    }

    if (o.do_all) {
        int n = djvu_doc_page_count(doc);
        for (i = 0; i < n; i++)
            rc |= run_page_ops(doc, ctx, &o, i);
    } else {
        rc |= run_page_ops(doc, ctx, &o, o.page - 1);
    }

done:
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    free(data);
    return rc;
}