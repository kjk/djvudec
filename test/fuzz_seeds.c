/* fuzz_seeds.c -- extract per-target fuzz seeds from real .djvu files.
 *
 *   fuzz_seeds <out_root> file.djvu ...
 *
 * Writes <out_root>/<target>/<fnv64 hex> for the per-codec libFuzzer targets,
 * in each target's input format (see test/fuzz_<target>.c):
 *   jb2      [u8 opts][u24 n][Djbz][u24 m][FGbz][Sjbz]   (inline or INCL dict)
 *   iw44     [u8 opts]([u24 n][chunk])...  BG44 / FG44 / PM44 / BM44 layers
 *   bzz      raw TXTz / ANTz / NAVM / DIRM-tail streams
 *   text     [u8 opts][decoded TXTz or TXTa payload]
 *   annot    decoded ANTz / ANTa (page or INCL'd shared annotation)
 *   outline  decoded NAVM
 *   doc      every page that stands alone (no INCL) as a single-page
 *            FORM:DJVU file -- small, valid documents for the doc target
 * Content-hash names dedupe across files. Seeds above FZ_SEED_MAX bytes are
 * skipped (IW44 layers are truncated to their leading chunks instead).
 * Driven by cmd/fuzz.ts, which runs it when a target's corpus is empty. */
#include "harness_util.h"
#include "djvu_internal.h"

#define FZ_SEED_MAX (256 * 1024)
#define FZ_MAX_PAGES_PER_FILE 12

static const char *g_root;
static int g_written;

static void put_u24(uint8_t *p, size_t v)
{
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

static void write_seed(const char *target, const uint8_t *d, size_t n)
{
    char path[4096];
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    FILE *f;
    if (n == 0 || n > FZ_SEED_MAX) return;
    for (i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    snprintf(path, sizeof(path), "%s/%s/%016llx", g_root, target,
             (unsigned long long)h);
    f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite(d, 1, n, f);
    fclose(f);
    g_written++;
}

/* Concatenate parts into one seed. */
typedef struct { const uint8_t *p; size_t n; } part;

static void write_parts(const char *target, const part *parts, int np)
{
    size_t total = 0, off = 0;
    uint8_t *buf;
    int i;
    for (i = 0; i < np; i++) total += parts[i].n;
    if (total == 0 || total > FZ_SEED_MAX) return;
    buf = (uint8_t *)malloc(total);
    if (!buf) return;
    for (i = 0; i < np; i++) {
        if (parts[i].n) memcpy(buf + off, parts[i].p, parts[i].n);
        off += parts[i].n;
    }
    write_seed(target, buf, total);
    free(buf);
}

static const uint8_t *find_dict(djvu_doc *doc, uint32_t form_off, uint32_t *sz)
{
    const uint8_t *p = djvu_form_find_chunk(doc, form_off, "Djbz", sz, NULL);
    if (!p) p = djvu_form_find_incl_chunk(doc, form_off, "Djbz", sz);
    return p;
}

static void seed_jb2(djvu_doc *doc, uint32_t form_off)
{
    uint32_t ssz = 0, dsz = 0, fsz = 0;
    const uint8_t *sjbz = djvu_form_find_chunk(doc, form_off, "Sjbz", &ssz, NULL);
    const uint8_t *djbz, *fgbz;
    uint8_t opts = 0, dl[3], fl[3];
    part parts[6];
    if (!sjbz) return;
    djbz = find_dict(doc, form_off, &dsz);
    fgbz = djvu_form_find_chunk(doc, form_off, "FGbz", &fsz, NULL);
    if (!djbz) dsz = 0;
    if (!fgbz) fsz = 0;
    put_u24(dl, dsz);
    put_u24(fl, fsz);
    parts[0].p = &opts; parts[0].n = 1;
    parts[1].p = dl; parts[1].n = 3;
    parts[2].p = djbz; parts[2].n = dsz;
    parts[3].p = fl; parts[3].n = 3;
    parts[4].p = fgbz; parts[4].n = fsz;
    parts[5].p = sjbz; parts[5].n = ssz;
    write_parts("jb2", parts, 6);
}

/* IW44 seeds keep only the leading chunks up to this size: decode cost is
   proportional to the coded bytes, and the corpus is replayed at every start. */
#define FZ_IW44_SEED_MAX (16 * 1024)

static void seed_iw44(djvu_doc *doc, uint32_t form_off, const char *id, uint8_t opts)
{
    uint8_t *buf = (uint8_t *)malloc(FZ_IW44_SEED_MAX);
    size_t n = 1;
    uint32_t start = 0, sz;
    const uint8_t *c;
    if (!buf) return;
    buf[0] = opts;
    while ((c = djvu_form_find_chunk(doc, form_off, id, &sz, &start)) != NULL) {
        if (n + 3 + sz > FZ_IW44_SEED_MAX) break; /* keep the leading chunks */
        put_u24(buf + n, sz);
        memcpy(buf + n + 3, c, sz);
        n += 3 + sz;
    }
    if (n > 1) write_seed("iw44", buf, n);
    free(buf);
}

/* BZZ chunk: raw stream to bzz, decoded payload to `target` (with optional
   1-byte prefix). */
static void seed_bzz_chunk(djvu_doc *doc, const uint8_t *c, uint32_t sz,
                           const char *target, int prefix)
{
    size_t dlen;
    uint8_t *dec;
    write_seed("bzz", c, sz);
    if (!target) return;
    dec = djvu_bzz_decode_all(doc->ctx, c, sz, &dlen);
    if (!dec) return;
    if (prefix >= 0) {
        uint8_t p = (uint8_t)prefix;
        part parts[2] = { { &p, 1 }, { dec, dlen } };
        write_parts(target, parts, 2);
    } else {
        write_seed(target, dec, dlen);
    }
    djvu_free(doc->ctx, dec);
}

static void seed_meta(djvu_doc *doc, uint32_t form_off)
{
    uint32_t sz;
    const uint8_t *c;
    if ((c = djvu_form_find_chunk(doc, form_off, "TXTz", &sz, NULL)))
        seed_bzz_chunk(doc, c, sz, "text", 0x33);
    if ((c = djvu_form_find_chunk(doc, form_off, "TXTa", &sz, NULL))) {
        uint8_t p = 0x33;
        part parts[2] = { { &p, 1 }, { c, sz } };
        write_parts("text", parts, 2);
    }
    if ((c = djvu_form_find_chunk(doc, form_off, "ANTz", &sz, NULL)))
        seed_bzz_chunk(doc, c, sz, "annot", -1);
    if ((c = djvu_form_find_chunk(doc, form_off, "ANTa", &sz, NULL)))
        write_seed("annot", c, sz);
    if ((c = djvu_form_find_incl_chunk(doc, form_off, "ANTz", &sz)))
        seed_bzz_chunk(doc, c, sz, "annot", -1);
}

/* A page FORM with no INCL is a complete document on its own. */
static void seed_doc_page(djvu_doc *doc, const djvu_page_int *pg)
{
    uint32_t sz;
    size_t n = (size_t)pg->form_size + 8;
    uint8_t *buf;
    if (djvu_form_find_chunk(doc, pg->form_off, "INCL", &sz, NULL)) return;
    if ((size_t)pg->form_off + n > doc->len || n + 4 > FZ_SEED_MAX) return;
    buf = (uint8_t *)malloc(n + 4);
    if (!buf) return;
    memcpy(buf, "AT&T", 4);
    memcpy(buf + 4, doc->data + pg->form_off, n);
    write_seed("doc", buf, n + 4);
    free(buf);
}

static void seed_file(const char *path)
{
    size_t len;
    uint8_t *data = hu_read_file(path, &len);
    djvu_ctx *ctx;
    djvu_doc *doc;
    uint32_t sz;
    const uint8_t *c;
    int i, np;
    if (!data) { fprintf(stderr, "cannot read %s\n", path); return; }
    ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, NULL, NULL);
    doc = ctx ? djvu_doc_open(ctx, data, len) : NULL;
    if (!doc) {
        if (ctx) djvu_ctx_free(ctx);
        free(data);
        return;
    }
    if ((c = djvu_form_find_chunk(doc, doc->root_form_off, "NAVM", &sz, NULL)))
        seed_bzz_chunk(doc, c, sz, "outline", -1);
    if ((c = djvu_form_find_chunk(doc, doc->root_form_off, "DIRM", &sz, NULL)) &&
        sz > 3) {
        /* DIRM: flags, u16 count, [bundled: u32 offsets], then BZZ */
        uint32_t skip = 3 + ((c[0] & 0x80) ? 4u * djvu_rd_u16be(c + 1) : 0);
        if (skip < sz) write_seed("bzz", c + skip, sz - skip);
    }
    np = doc->npages;
    if (np > FZ_MAX_PAGES_PER_FILE) np = FZ_MAX_PAGES_PER_FILE;
    for (i = 0; i < np; i++) {
        const djvu_page_int *pg = &doc->pages[i];
        const char *bgid = djvu_form_bg_iw44_id(doc, pg->form_off);
        seed_jb2(doc, pg->form_off);
        if (bgid) seed_iw44(doc, pg->form_off, bgid, 0);
        if (bgid) seed_iw44(doc, pg->form_off, bgid, 1);
        seed_iw44(doc, pg->form_off, "FG44", 0);
        seed_meta(doc, pg->form_off);
        seed_doc_page(doc, pg);
    }
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);
    free(data);
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: fuzz_seeds <out_root> file.djvu ...\n");
        return 2;
    }
    g_root = argv[1];
    for (i = 2; i < argc; i++) seed_file(argv[i]);
    printf("fuzz_seeds: wrote %d seed(s)\n", g_written);
    return 0;
}
