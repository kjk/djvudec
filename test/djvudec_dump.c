/* djvudec_dump.c -- inspect .djvu files using the djvudec library only.
 *
 * Public API (djvu.h) examples: djvu_init, -info, -text, -zones, -outline,
 * -links, -type,
 * -id, -title, -resolve, -out (render). Codec/layer introspection uses
 * djvu_internal.h helpers (-comps, -bzzdec, -iw*, -bg).
 *
 *   djvudec_dump -info file.djvu
 *   djvudec_dump -page 3 -zones -text file.djvu
 *   djvudec_dump -profile 20 -page 5 file.djvu   # winperf section marks
 *
 * No DjVuLibre dependency; links only src/ sources + this file. */
#include "harness_util.h"
#include "djvu_internal.h"
#if defined(_WIN32)
/* Vendored control client; calls are no-ops when winperf is not recording. */
#include "winperf_control.h"
#else
static void winperf_profile_start(void) {}
static void winperf_profile_stop(void) {}
#endif

/* Timing uses djvu_bench_now_ms throughout: the same clock the decoder's own
   per-layer timings (djvu_page_render_timed) are taken with. */

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

/* -sumatra (1) / -sumatra-cached (2): see usage */
static int g_sumatra_mode;

/* One timed render the SumatraPDF way: djvu_page_render_into a BGR caller
   buffer (sized from djvu_page_render_info, allocated and touched outside the
   timer, like a reused DIB). Mode 1 drops the page cache first (first paint). */
static int bench_render_page_sumatra(djvu_doc *doc, int page0, int warm, int sub,
                                     double *total_ms)
{
    djvu_render_info ri;
    uint8_t *dst;
    int stride, w, rc;
    double t0;

    if (djvu_page_render_info(doc, page0, sub, &ri) != 0) return 1;
    stride = ri.width * (ri.format == DJVU_FORMAT_GRAY8 ? 1 : 3);
    dst = (uint8_t *)malloc((size_t)stride * (size_t)ri.height);
    if (!dst) return 1;
    memset(dst, 0, (size_t)stride * (size_t)ri.height);
    for (w = 0; w < warm; w++)
        if (djvu_page_render_into(doc, page0, sub, dst, stride) != 0) { free(dst); return 1; }
    if (g_sumatra_mode == 1) djvu_doc_drop_page_cache(doc, page0);
    t0 = djvu_bench_now_ms();
    rc = djvu_page_render_into(doc, page0, sub, dst, stride);
    *total_ms = djvu_bench_now_ms() - t0;
    free(dst);
    return rc != 0;
}

static int bench_render_page(djvu_doc *doc, int page0, int warm, int sub,
                             double *total_ms, djvu_render_timings *layer)
{
    djvu_ctx *ctx = doc->ctx;
    int w;
    double t0;

    if (g_sumatra_mode)
        return bench_render_page_sumatra(doc, page0, warm, sub, total_ms);

    for (w = 0; w < warm; w++) {
        djvu_image *img = djvu_page_render_timed(doc, page0, sub, NULL);
        if (!img) return 1;
        djvu_image_destroy(ctx, img);
    }

    t0 = djvu_bench_now_ms();
    {
        djvu_image *img = djvu_page_render_timed(doc, page0, sub, layer);
        if (!img) return 1;
        djvu_image_destroy(ctx, img);
    }
    *total_ms = djvu_bench_now_ms() - t0;
    return 0;
}

/* Loop page renders with winperf section marks around each render.
 * Doc stays open; marks bracket only djvu_page_render so open/IO is excluded. */
static int run_profile_render(djvu_doc *doc, int loops, int sub, int page0)
{
    djvu_ctx *ctx = doc->ctx;
    int npages = djvu_doc_page_count(doc);
    int i, w = 0, h = 0;
    double total = 0.0;
    djvu_page_info info;

    if (loops < 1) loops = 1;
    if (sub < 1) sub = 1;
    if (page0 < 0 || page0 >= npages) {
        fprintf(stderr, "invalid page %d (document has %d pages)\n",
                page0 + 1, npages);
        return 1;
    }
    memset(&info, 0, sizeof(info));
    if (djvu_doc_page_info(doc, page0, &info) == 0) {
        w = info.width;
        h = info.height;
    }

    for (i = 0; i < loops; i++) {
        djvu_image *img;
        double t0;

        winperf_profile_start();
        t0 = djvu_bench_now_ms();
        img = djvu_page_render(doc, page0, sub);
        total += djvu_bench_now_ms() - t0;
        winperf_profile_stop();
        if (!img) {
            fprintf(stderr, "profile: render failed on loop %d page %d\n",
                    i + 1, page0 + 1);
            return 1;
        }
        if (img->width > 0) w = img->width;
        if (img->height > 0) h = img->height;
        djvu_image_destroy(ctx, img);
    }
    printf("profile loops=%d page=%d sub=%d size=%dx%d total_ms=%.2f avg_ms=%.2f\n",
           loops, page0 + 1, sub, w, h, total, total / (double)loops);
    return 0;
}

/* Loop complete decoder sessions with winperf section marks around
 * djvu_doc_open + render every page + djvu_doc_close. The input buffer and
 * context stay alive, matching the public API's in-memory ownership model. */
static int run_profile_document(djvu_ctx *ctx, const uint8_t *data, size_t len,
                                int loops, int sub)
{
    int i, p, npages = 0;
    double total = 0.0;

    if (loops < 1) loops = 1;
    if (sub < 1) sub = 1;

    for (i = 0; i < loops; i++) {
        djvu_doc *doc;
        double t0;

        winperf_profile_start();
        t0 = djvu_bench_now_ms();
        doc = djvu_doc_open(ctx, data, len);
        if (!doc) {
            winperf_profile_stop();
            fprintf(stderr, "profile-document: open failed on loop %d\n", i + 1);
            return 1;
        }
        npages = djvu_doc_page_count(doc);
        for (p = 0; p < npages; p++) {
            djvu_image *img = djvu_page_render(doc, p, sub);
            if (!img) {
                djvu_doc_close(doc);
                winperf_profile_stop();
                fprintf(stderr,
                        "profile-document: render failed on loop %d page %d\n",
                        i + 1, p + 1);
                return 1;
            }
            djvu_image_destroy(ctx, img);
        }
        djvu_doc_close(doc);
        total += djvu_bench_now_ms() - t0;
        winperf_profile_stop();
    }
    printf("profile-document loops=%d pages=%d sub=%d total_ms=%.2f avg_ms=%.2f\n",
           loops, npages, sub, total, total / (double)loops);
    return 0;
}

/* Two timed renders per page (djvudec only; for bench_before/after). */
static int run_bench_render(djvu_doc *doc, int warm, int reps, int layers,
                            int sub, int page0, int single_page)
{
    int npages = djvu_doc_page_count(doc);
    int lo, hi, i, r;
    if (reps < 1) reps = 2;

    if (single_page) {
        if (page0 < 0 || page0 >= npages) {
            fprintf(stderr, "invalid page %d (document has %d pages)\n",
                    page0 + 1, npages);
            return 1;
        }
        lo = hi = page0;
    } else {
        lo = 0;
        hi = npages - 1;
    }

    for (i = lo; i <= hi; i++) {
        double *t = (double *)calloc((size_t)reps, sizeof(*t));
        djvu_render_timings *lt = layers
            ? (djvu_render_timings *)calloc((size_t)reps, sizeof(*lt))
            : NULL;
        if (!t || (layers && !lt)) {
            free(t);
            free(lt);
            return 1;
        }

        for (r = 0; r < reps; r++) {
            if (bench_render_page(doc, i, warm, sub, &t[r], layers ? &lt[r] : NULL) != 0)
            {
                free(t);
                free(lt);
                return 1;
            }
        }
        printf("p%d", i + 1);
        for (r = 0; r < reps; r++)
            printf(" %.2f", t[r]);
        printf("\n");
        if (layers) {
            printf("layer p%d jb2", i + 1);
            for (r = 0; r < reps; r++) printf(" %.2f", lt[r].jb2_ms);
            printf(" iw44");
            for (r = 0; r < reps; r++) printf(" %.2f", lt[r].iw44_ms);
            printf(" composite");
            for (r = 0; r < reps; r++) printf(" %.2f", lt[r].composite_ms);
            printf(" rotate");
            for (r = 0; r < reps; r++) printf(" %.2f", lt[r].rotate_ms);
            printf("\n");
        }
        free(t);
        free(lt);
    }
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
    int do_bench_render;
    int do_cache_probe;
    int do_layers;
    int bench_warm;
    int bench_reps;
    int bench_sub;
    int profile_loops; /* >0: -profile N (winperf marks around each render) */
    int profile_document_loops; /* >0: profile open + all pages + close */
    int page_explicit;
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
        "  -bench-render      time 2 renders/page (pN t1 t2 ms; djvudec only)\n"
        "  -page N, -p N      with -bench-render/-profile: single page (default: all / 1)\n"
        "  -warm N            discard first N renders/page before timing (default 0)\n"
        "  -reps N            timed renders/page for -bench-render (default 2)\n"
        "  -sub N             with -bench-render/-profile: subsample N (default 1)\n"
        "  -layers            with -bench-render: per-stage jb2/iw44/composite/rotate\n"
        "  -sumatra           with -bench-render: render like SumatraPDF (BGR, page\n"
        "                     cache + lock hooks, render_into a reused buffer); the\n"
        "                     page cache is dropped before each timed render\n"
        "  -sumatra-cached    same, keeping the page cache (repaint from cache)\n"
        "  -profile N         render page N times with winperf section marks (see\n"
        "                     bun cmd/prof.ts); needs -page for multipage files\n"
        "  -profile-document N  open, render every page, close N times with\n"
        "                       winperf section marks\n"
        "  -cache-probe       enable per-page cache; report size after first render and drop\n"
        "\n"
        "Codec layers (no full composite):\n"
        "  -bzzdec [-out FILE] decode raw BZZ stream to FILE or stdout (no document open)\n"
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
    o->bench_reps = 2;

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
        else if (!strcmp(argv[i], "-bench-render")) o->do_bench_render = 1;
        else if (!strcmp(argv[i], "-cache-probe")) o->do_cache_probe = 1;
        else if (!strcmp(argv[i], "-sumatra")) g_sumatra_mode = 1;
        else if (!strcmp(argv[i], "-sumatra-cached")) g_sumatra_mode = 2;
        else if (!strcmp(argv[i], "-layers")) o->do_layers = 1;
        else if (!strcmp(argv[i], "-profile") && i + 1 < argc) {
            o->profile_loops = atoi(argv[++i]);
            if (o->profile_loops < 1) o->profile_loops = 1;
        } else if (!strcmp(argv[i], "-profile-document") && i + 1 < argc) {
            o->profile_document_loops = atoi(argv[++i]);
            if (o->profile_document_loops < 1) o->profile_document_loops = 1;
        } else if (!strcmp(argv[i], "-warm") && i + 1 < argc)
            o->bench_warm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-reps") && i + 1 < argc)
            o->bench_reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sub") && i + 1 < argc)
            o->bench_sub = atoi(argv[++i]);
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
        } else if ((!strcmp(argv[i], "-page") || !strcmp(argv[i], "-p")) && i + 1 < argc) {
            o->page = atoi(argv[++i]);
            o->page_explicit = 1;
        } else if (!strcmp(argv[i], "-out") && i + 1 < argc)
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
        printf("page %d type: %s\n", page0 + 1, hu_page_type_name(t));
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
        if (z && z->root) hu_print_zone(z->root, 0);
        else printf("(no text zones)\n");
        djvu_text_zones_destroy(ctx, z);
    }

    if (o->do_links) {
        djvu_page_links *L = djvu_page_get_links(doc, page0);
        if (o->do_all) printf("--- page %d links ---\n", page0 + 1);
        hu_print_links(L);
        if (L) djvu_page_links_destroy(ctx, L);
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
                    if (hu_write_pnm(o->out, img, 0) != 0) rc = 1;
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
            if (hu_write_pnm(o->out, img, 0) != 0) {
                fprintf(stderr, "cannot write %s\n", o->out);
                rc = 1;
            }
            djvu_image_destroy(ctx, img);
        } else rc = 1;
    }

    return rc;
}

/* Lazy per-page cache: size before/after first render and after drop. */
static int run_cache_probe(djvu_doc *doc, int page0)
{
    djvu_ctx *ctx = doc->ctx;
    size_t before, after, dropped;
    djvu_image *img;
    int p1 = page0 + 1;

    if (page0 < 0 || page0 >= djvu_doc_page_count(doc)) {
        fprintf(stderr, "invalid page %d\n", p1);
        return 1;
    }
    before = djvu_doc_page_cache_size(doc, page0);
    img = djvu_page_render(doc, page0, 1);
    if (!img) {
        fprintf(stderr, "render failed for page %d\n", p1);
        return 1;
    }
    djvu_image_destroy(ctx, img);
    after = djvu_doc_page_cache_size(doc, page0);
    djvu_doc_drop_page_cache(doc, page0);
    dropped = djvu_doc_page_cache_size(doc, page0);
    printf("cache-probe page %d: before=%zu after_render=%zu after_drop=%zu\n",
           p1, before, after, dropped);
    if (before != 0) {
        fprintf(stderr, "cache-probe: expected 0 bytes before first render\n");
        return 1;
    }
    {
        djvu_page_type t = djvu_page_get_type(doc, page0);
        if (t != DJVU_PAGE_UNKNOWN && after == 0) {
            fprintf(stderr, "cache-probe: expected non-zero cache after first "
                            "render of typed page (type=%d)\n", (int)t);
            return 1;
        }
    }
    if (dropped != 0) {
        fprintf(stderr, "cache-probe: expected 0 bytes after drop\n");
        return 1;
    }
    return 0;
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

    data = hu_read_file(o.in, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", o.in);
        return 1;
    }

    djvu_init();
    if (o.do_cache_probe || g_sumatra_mode)
        ctx = djvu_ctx_new(NULL, NULL, hu_lock_cb, hu_unlock_cb, hu_on_error, NULL);
    else
        ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, hu_on_error, NULL);
    if (!ctx) { free(data); return 1; }
    if (o.do_cache_probe || g_sumatra_mode)
        djvu_ctx_set_cache_per_page(ctx, 1);
    if (g_sumatra_mode)
        djvu_ctx_set_bgr(ctx, 1);

    if (o.do_bzz) {
        size_t olen = 0;
        uint8_t *out = djvu_bzz_decode_all(ctx, data, len, &olen);
        if (!out) rc = 1;
        else {
            FILE *f = o.out ? fopen(o.out, "wb") : stdout;
            if (!o.out) hu_stdout_binary(); /* raw bytes: no CRLF translation */
            if (!f) {
                fprintf(stderr, "cannot write %s\n", o.out);
                rc = 1;
            } else {
                if (fwrite(out, 1, olen, f) != olen) rc = 1;
                if (o.out ? fclose(f) != 0 : fflush(f) != 0) rc = 1;
            }
            djvu_free(ctx, out);
        }
        djvu_ctx_free(ctx);
        free(data);
        return rc;
    }

    if (o.profile_document_loops > 0) {
        rc = run_profile_document(ctx, data, len, o.profile_document_loops,
                                  o.bench_sub < 1 ? 1 : o.bench_sub);
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

    if (o.profile_loops > 0) {
        rc = run_profile_render(doc, o.profile_loops,
                                o.bench_sub < 1 ? 1 : o.bench_sub,
                                o.page - 1);
        goto done;
    }

    if (o.do_bench_render) {
        rc = run_bench_render(doc, o.bench_warm, o.bench_reps, o.do_layers,
                              o.bench_sub < 1 ? 1 : o.bench_sub,
                              o.page - 1, o.page_explicit);
        goto done;
    }

    if (o.do_cache_probe) {
        if (o.do_all) {
            int n = djvu_doc_page_count(doc);
            for (i = 0; i < n; i++)
                rc |= run_cache_probe(doc, i);
        } else {
            rc = run_cache_probe(doc, o.page - 1);
        }
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
        if (root) hu_print_outline(root, -1);
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
