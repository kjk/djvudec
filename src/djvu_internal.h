/* djvu_internal.h -- internal structures and helpers (all modules).
 *
 * This is the single internal header for the C DjVu decoder. It bundles what
 * used to be separate per-module headers (zp, bitmap, bzz, jb2, iw44) so the
 * .c files only need to include this one file. Sections below are grouped by
 * module; declarations within each match the corresponding .c file. */
#ifndef DJVU_INTERNAL_H
#define DJVU_INTERNAL_H

#include "djvu.h"
#include <stdarg.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* ===================================================================== */
/* core: context, document, chunk parsing, byte readers                  */
/* ===================================================================== */

struct djvu_ctx {
    djvu_alloc_cb alloc;
    djvu_free_cb  free;
    djvu_error_cb error;
    void *user;
    int lazy_iw44;     /* defer IW44/JB2 preload until explicit preload or use */
    int no_compose;    /* skip color composite in render */
    int iw_max_chunks; /* cap IW44 chunks per layer (0 = unlimited) */
    int bgr;           /* emit color output as B,G,R instead of R,G,B */
};

void *djvu_alloc(djvu_ctx *ctx, size_t size);
void  djvu_free(djvu_ctx *ctx, void *ptr);
void  djvu_errorf(djvu_ctx *ctx, djvu_severity sev, const char *fmt, ...);

/* Monotonic clock for bench_render / -layers (ms). */
static inline double djvu_bench_now_ms(void)
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

/* A component listed in the DJVM directory (DIRM). */
typedef struct {
    uint32_t offset;     /* file offset of the component's "FORM" tag */
    uint32_t size;       /* component size from DIRM (0 if unknown) */
    char    *id;         /* component id used by INCL refs (NUL-term, owned) */
    char    *title;      /* component title from DIRM, or NULL (owned) */
    int      type;       /* 0=include(DJVI) 1=page 2=thumb 3=shared-anno */
} djvu_component;

typedef struct iw_pixmap iw_pixmap;
typedef struct jb2_image jb2_image;

/* Cached shared Djbz dictionary (keyed by INCL component id). */
typedef struct {
    char *incl_id;       /* owned; matches INCL chunk / DIRM component id */
    jb2_image *dict;     /* decoded Djbz (doc-owned; read-only during render) */
} djvu_jb2_dict_entry;

/* Cached inline Djbz (deduped by identical chunk bytes). */
typedef struct {
    const uint8_t *djbz; /* chunk body in doc->data (borrowed) */
    uint32_t djbz_sz;
    jb2_image *dict;     /* decoded once; shared by all matching pages */
} djvu_jb2_inline_entry;

/* A displayable page = a FORM:DJVU component in the document. */
typedef struct {
    uint32_t form_off;   /* file offset of the "FORM" tag */
    uint32_t form_size;  /* size field of that FORM chunk */
    int has_info;
    djvu_page_info info;
    const char *id;      /* directory component id (borrowed from comps) */
    const char *title;   /* directory component title (borrowed), or NULL */
    iw_pixmap *iw_bg;    /* decoded BG44 (filled at doc open; doc-owned) */
    iw_pixmap *iw_fg;    /* decoded FG44 (filled at doc open; doc-owned) */
    jb2_image *jb2_dict; /* borrowed inline Djbz cache entry (doc open; do not free) */
    jb2_image *jb2_mask; /* decoded Sjbz mask (filled at doc open; doc-owned) */
} djvu_page_int;

struct djvu_doc {
    djvu_ctx *ctx;
    const uint8_t *data;
    size_t len;
    uint32_t root_form_off;  /* offset of the outer FORM (DJVU/DJVM) tag */
    int npages;
    djvu_page_int *pages;
    int ncomp;
    djvu_component *comps;   /* all DIRM components, in directory order */
    djvu_jb2_dict_entry *jb2_dicts;
    int n_jb2_dicts;
    djvu_jb2_inline_entry *jb2_inline;
    int n_jb2_inline;
};

/* Cached IW44 layers (read-only during render; freed in djvu_doc_close).
   With ctx->lazy_iw44 at doc open, layers decode on first use per page. */
iw_pixmap *djvu_doc_iw44(djvu_doc *doc, int page_no, const char *chunk_id);
iw_pixmap *djvu_doc_iw44_by_form(djvu_doc *doc, uint32_t form_off, const char *chunk_id);
void djvu_doc_drop_page_iw44(djvu_doc *doc, int page_no);
void djvu_doc_preload_iw44_range(djvu_doc *doc, int lo0, int hi0);
void djvu_doc_preload_jb2_range(djvu_doc *doc, int lo0, int hi0);
void djvu_doc_preload_jb2_masks_range(djvu_doc *doc, int lo0, int hi0);

/* Cached JB2 dictionaries (read-only during render; freed in djvu_doc_close). */
jb2_image *djvu_doc_jb2_mask(djvu_doc *doc, int page_no);
jb2_image *djvu_doc_jb2_dict(djvu_doc *doc, const char *incl_id);
jb2_image *djvu_doc_jb2_dict_inline(djvu_doc *doc, uint32_t form_off);
jb2_image *djvu_doc_jb2_dict_for_form(djvu_doc *doc, uint32_t form_off);
/* Resolve an INCL component id to its FORM file offset; 0 if not found. */
uint32_t djvu_doc_component_offset(djvu_doc *doc, const char *id);

/* Find the first sub-chunk with id `id` inside the FORM at form_off.
   Returns a pointer to the chunk data and sets *out_size, or NULL.
   If `start` != NULL, *start is used as the byte offset to begin scanning and
   is advanced past the found chunk (to iterate repeated chunks like INCL). */
const uint8_t *djvu_form_find_chunk(djvu_doc *doc, uint32_t form_off,
                                    const char *id, uint32_t *out_size,
                                    uint32_t *start);

/* Trim trailing whitespace/control from an INCL component id (in-place). */
void djvu_trim_incl_id(char *s);

/* Find chunk_id in a component referenced by an INCL chunk in form_off.
   Checks every INCL in order; returns NULL if none reference the chunk. */
const uint8_t *djvu_form_find_incl_chunk(djvu_doc *doc, uint32_t form_off,
                                         const char *chunk_id, uint32_t *out_size);

/* big-endian / little-endian readers over the file buffer (bounds-checked
   by callers; return 0 past end) */
static inline uint32_t djvu_rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t djvu_rd_u24be(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static inline uint32_t djvu_rd_u16be(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}
static inline uint32_t djvu_rd_u16le(const uint8_t *p) {
    return ((uint32_t)p[1] << 8) | p[0];
}

/* Does a 4-byte tag at p equal `tag`? */
static inline int djvu_tag_eq(const uint8_t *p, const char *tag) {
    return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
           p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

/* Bottom-up page Y (lower-left origin) -> top-down (y from top edge). */
static inline int djvu_y_bottomup_to_topdown(int y, int page_h, int h) {
    return page_h - (y + h);
}

/* Copy bottom-up RGB (row 0 = bottom) into top-down dst (w*h*3 bytes). */
/* Flip a bottom-up RGB pixmap to top-down. When bgr != 0, also swap R<->B so
   the output is in B,G,R order (matches a Windows DIB / DDJVU_FORMAT_BGR24). */
void djvu_flip_rgb_bottomup(uint8_t *dst, const uint8_t *src, int w, int h, int bgr);

/* Bounded buffer reader for NAVM/outline and text-zone payloads. */
typedef struct {
    djvu_ctx *ctx;
    const uint8_t *p;
    size_t len, pos;
    int failed;
} djvu_buf_reader;

static inline void djvu_br_init(djvu_buf_reader *br, djvu_ctx *ctx,
                                const uint8_t *p, size_t len)
{
    br->ctx = ctx;
    br->p = p;
    br->len = len;
    br->pos = 0;
    br->failed = 0;
}

static inline int djvu_br_u8(djvu_buf_reader *br)
{
    if (br->pos >= br->len) { br->failed = 1; return 0; }
    return br->p[br->pos++];
}

static inline int djvu_br_u16be(djvu_buf_reader *br)
{
    int v;
    if (br->pos + 2 > br->len) { br->failed = 1; return 0; }
    v = (int)djvu_rd_u16be(br->p + br->pos);
    br->pos += 2;
    return v;
}

static inline int djvu_br_u24be(djvu_buf_reader *br)
{
    int v;
    if (br->pos + 3 > br->len) { br->failed = 1; return 0; }
    v = (int)djvu_rd_u24be(br->p + br->pos);
    br->pos += 3;
    return v;
}

/* u16-BE biased by 0x8000 (text zone coordinates/offsets). */
static inline int djvu_br_s16be_biased(djvu_buf_reader *br)
{
    int v;
    if (br->pos + 2 > br->len) { br->failed = 1; return 0; }
    v = (int)djvu_rd_u16be(br->p + br->pos) - 0x8000;
    br->pos += 2;
    return v;
}

char *djvu_br_strdup(djvu_buf_reader *br, int slen);

/* ===================================================================== */
/* zpcodec.c -- ZP-Coder binary adaptive arithmetic decoder (decode only) */
/* Ported from DjvuNet Compression/ZPCodec.cs.                            */
/* ===================================================================== */

typedef struct {
    uint16_t p;   /* PValue */
    uint16_t m;   /* MValue */
    uint8_t  up;
    uint8_t  dn;  /* down */
} djvu_zp_table;

extern const djvu_zp_table djvu_zp_default_table[256];

/* A BitContext: a single adaptive state byte. Callers keep arrays of these. */
typedef uint8_t djvu_zp_ctx;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;

    uint32_t a;       /* "avalue" */
    uint32_t code;
    uint32_t fence;
    uint32_t buffer;
    uint8_t  scount;
    uint8_t  delay;
    uint8_t  zbyte;
    int      eof;     /* set when delay underflows (matches C# EOF behaviour) */

    uint32_t p[256];
    uint32_t m[256];
    uint8_t  up[256];
    uint8_t  dn[256];
    int8_t   ffzt[256];
} djvu_zp;

/* Initialize a decoder over [data, data+len). */
void djvu_zp_init(djvu_zp *zp, const uint8_t *data, size_t len);

/* ZP fast path (static inline so JB2/BZZ/IW44 loops can inline djvu_zp_decode). */
static inline int zp_read_byte(djvu_zp *zp)
{
    if (zp->pos < zp->len)
        return zp->data[zp->pos++];
    return -1;
}

static inline void zp_preload(djvu_zp *zp)
{
    unsigned scount = zp->scount;
    int zbyte = zp->zbyte;
    uint32_t buffer = zp->buffer;

    for (; scount <= 24; scount += 8) {
        int b = zp_read_byte(zp);
        if (b == -1) {
            zbyte = 255;
            if (--zp->delay < 1)
                zp->eof = 1;
        } else {
            zbyte = b;
        }
        buffer = (buffer << 8) | (uint8_t)zbyte;
    }

    zp->scount = (uint8_t)scount;
    zp->zbyte = (uint8_t)zbyte;
    zp->buffer = buffer;
}

static inline int zp_ffz(djvu_zp *zp, uint32_t x)
{
    return ((x & 0xffffffffu) < 0xff00u)
        ? zp->ffzt[(x >> 8) & 0xff]
        : (zp->ffzt[x & 0xff] + 8);
}

static inline void zp_renorm_lps(djvu_zp *zp, uint32_t z)
{
    int shift;
    z = 0x10000u - z;
    zp->a += z;
    zp->code += z;
    shift = zp_ffz(zp, zp->a);
    zp->scount -= (uint8_t)shift;
    zp->a = (zp->a << shift) & 0xffff;
    zp->code = ((zp->code << shift) & 0xffff)
             | ((zp->buffer >> zp->scount) & ((1u << shift) - 1));
    if (zp->scount < 16)
        zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000)
        zp->fence = 0x7fff;
}

static inline void zp_renorm_mps(djvu_zp *zp, uint32_t z)
{
    zp->scount -= 1;
    zp->a = (z << 1) & 0xffff;
    zp->code = ((zp->code << 1) & 0xffff) | ((zp->buffer >> zp->scount) & 1);
    if (zp->scount < 16)
        zp_preload(zp);
    zp->fence = zp->code;
    if (zp->code >= 0x8000)
        zp->fence = 0x7fff;
}

static inline int zp_decode_sub(djvu_zp *zp, djvu_zp_ctx *ctx, uint32_t z)
{
    int bit = *ctx & 1;
    uint32_t d = 0x6000u + ((z + zp->a) >> 2);
    if (z > d)
        z = d;

    if (z > zp->code) {
        *ctx = zp->dn[*ctx];
        zp_renorm_lps(zp, z);
        return bit ^ 1;
    } else {
        if (zp->a >= zp->m[*ctx])
            *ctx = zp->up[*ctx];
        zp_renorm_mps(zp, z);
        return bit;
    }
}

static inline int djvu_zp_decode(djvu_zp *zp, djvu_zp_ctx *ctx)
{
    uint32_t z = zp->a + zp->p[*ctx];
    if (z <= zp->fence) {
        zp->a = z;
        return *ctx & 1;
    }
    return zp_decode_sub(zp, ctx, z);
}

static inline int zp_decode_sub_simple(djvu_zp *zp, int mps, uint32_t z)
{
    if (z > zp->code) {
        zp_renorm_lps(zp, z);
        return mps ^ 1;
    } else {
        zp_renorm_mps(zp, z);
        return mps;
    }
}

static inline int djvu_zp_decode_pass(djvu_zp *zp)
{
    return zp_decode_sub_simple(zp, 0, 0x8000u + (zp->a >> 1));
}

static inline int djvu_zp_decode_iw(djvu_zp *zp)
{
    return zp_decode_sub_simple(zp, 0, 0x8000u + ((zp->a + zp->a + zp->a) >> 3));
}

/* ===================================================================== */
/* bitmap.c -- 8bpp gray bitmap with a context border (GBitmap port).     */
/* Used for JB2 shape/mask bitmaps. 0 = background(white), >0 = ink.       */
/* ===================================================================== */

typedef struct {
    int width, height;
    int border;
    int bytes_per_row;   /* width + border */
    int max_offset;      /* height*bytes_per_row + border = length of data */
    uint8_t *data;       /* raw pixels; NULL when RLE-compressed */
    uint8_t *guard;      /* zero row for row<0/row>=height (GBitmap zerobuffer) */
    uint8_t *rle;        /* run-length encoding (DjVuLibre GBitmap::compress) */
    size_t rle_len;
} djvu_bitmap;

static inline int djvu_bm_has_pixels(const djvu_bitmap *bm)
{
    return bm && bm->width > 0 && bm->height > 0 && (bm->data || bm->rle);
}

/* (re)initialize bm to height x width with the given context border (zeroed). */
int  djvu_bm_init(djvu_ctx *ctx, djvu_bitmap *bm, int height, int width, int border);
void djvu_bm_free(djvu_ctx *ctx, djvu_bitmap *bm);

static inline int djvu_bm_rowoffset(const djvu_bitmap *bm, int row) {
    return row * bm->bytes_per_row + bm->border;
}

/* JB2 bitmap-coding context (unchecked column reads; border/guard rows are zero). */
static inline int jb2_get_direct_context(const uint8_t *up2, const uint8_t *up1,
                                           const uint8_t *up0, int col)
{
    return (up2[col - 1] << 9) | (up2[col] << 8) | (up2[col + 1] << 7) |
           (up1[col - 2] << 6) | (up1[col - 1] << 5) | (up1[col] << 4) |
           (up1[col + 1] << 3) | (up1[col + 2] << 2) |
           (up0[col - 2] << 1) | (up0[col - 1]);
}

static inline int jb2_shift_direct_context(int ctx, int next, const uint8_t *up2,
                                           const uint8_t *up1, int col)
{
    return ((ctx << 1) & 0x37a) | (up1[col + 2] << 2) | (up2[col + 1] << 7) | next;
}

static inline int jb2_get_cross_context(const uint8_t *up1, const uint8_t *up0,
                                        const uint8_t *xup1, const uint8_t *xup0,
                                        const uint8_t *xdn1, int col)
{
    return (up1[col - 1] << 10) | (up1[col] << 9) | (up1[col + 1] << 8) |
           (up0[col - 1] << 7) | (xup1[col] << 6) | (xup0[col - 1] << 5) |
           (xup0[col] << 4) | (xup0[col + 1] << 3) |
           (xdn1[col - 1] << 2) | (xdn1[col] << 1) | (xdn1[col + 1]);
}

static inline int jb2_shift_cross_context(int ctx, int n, const uint8_t *up1,
                                          const uint8_t *xup1, const uint8_t *xup0,
                                          const uint8_t *xdn1, int col)
{
    return ((ctx << 1) & 0x636) | (up1[col + 1] << 8) | (xup1[col] << 6) |
           (xup0[col + 1] << 3) | (xdn1[col + 1]) | (n << 7);
}

/* grow the border to at least `value`, preserving pixels. */
int djvu_bm_set_min_border(djvu_ctx *ctx, djvu_bitmap *bm, int value);

/* tight bounding box of non-zero pixels; if empty, returns xmin>xmax. */
void djvu_bm_bbox(const djvu_bitmap *bm, int *xmin, int *ymin, int *xmax, int *ymax);

/* OR-blit src into dst at (dx,dy) (bottom-up coords), clamping to maxval. */
void djvu_bm_blit(djvu_bitmap *dst, const djvu_bitmap *src, int dx, int dy, int maxval);

/* DjVuLibre GBitmap::compress — drop raw bytes, keep RLE only. */
void djvu_bm_compress(djvu_ctx *ctx, djvu_bitmap *bm);
/* DjVuLibre GBitmap::uncompress — expand RLE to raw bytes. */
void djvu_bm_uncompress(djvu_ctx *ctx, djvu_bitmap *bm);
/* Uncompress when only RLE is present (no-op if bytes already exist). */
void djvu_bm_ensure_bytes(djvu_ctx *ctx, djvu_bitmap *bm);

/* Call fn(user, px, py) for each ink pixel at page offset (left, bottom). */
void djvu_bm_visit_ink(const djvu_bitmap *src, int left, int bottom,
                       void (*fn)(void *user, int px, int py), void *user);

/* ===================================================================== */
/* bzz.c -- BZZ (Burrows-Wheeler + ZP) decompression.                     */
/* Ported from DjvuNet Compression/BSInputStream.cs (decode path).        */
/* ===================================================================== */

/* Decompress an entire BZZ stream at [data, data+len) into a freshly
   allocated buffer (NUL-terminated for convenience; the NUL is not counted
   in *out_len). Returns NULL on error. Free with djvu_free(). */
uint8_t *djvu_bzz_decode_all(djvu_ctx *ctx, const uint8_t *data, size_t len,
                             size_t *out_len);

/* ===================================================================== */
/* jb2.c -- JB2 bitonal decoder (DjvuNet JB2 modules, decode only).       */
/* ===================================================================== */

typedef struct {
    int parent;
    djvu_bitmap bm;
} jb2_shape;

typedef struct { int left, bottom, shapeno; } jb2_blit;

typedef struct jb2_image {
    djvu_ctx *ctx;
    /* dictionary part */
    jb2_shape *shapes;
    int nshapes, cap_shapes;
    int inherited_shapes;
    struct jb2_image *inherited_dict;
    /* image part */
    int width, height;
    jb2_blit *blits;
    int nblits, cap_blits;
} jb2_image;

/* Decode a JB2 stream (Sjbz mask or Djbz dictionary) from [data,len).
   `dict` is the inherited shape dictionary (from a Djbz), or NULL.
   Returns a new jb2_image (free with djvu_jb2_free), NULL on error. */
jb2_image *djvu_jb2_decode(djvu_ctx *ctx, const uint8_t *data, size_t len,
                           jb2_image *dict);
/* Decode a Djbz shared dictionary stream (zero image size). */
jb2_image *djvu_jb2_decode_dict(djvu_ctx *ctx, const uint8_t *data, size_t len);
void djvu_jb2_free(djvu_ctx *ctx, jb2_image *img);

/* Resolve a shape number through the inheritance chain. */
jb2_shape *djvu_jb2_get_shape(jb2_image *img, int shapeno);

/* ===================================================================== */
/* iw44.c -- IW44 wavelet decoder (DjvuNet Wavelet port, decode only).    */
/* Decodes BG44/FG44/PM44/BM44 chunks into a Y[CbCr] pixmap.              */
/* ===================================================================== */

iw_pixmap *djvu_iw44_new(djvu_ctx *ctx);
void djvu_iw44_free(iw_pixmap *pm);

/* Feed one IW44 chunk (e.g. one BG44). Chunks must arrive in serial order.
   Returns 0 on success, -1 on error. */
int djvu_iw44_decode_chunk(iw_pixmap *pm, const uint8_t *data, size_t len);

/* Decode chunk_id chunks (e.g. "BG44") from a page form into pm.
   max_chunks <= 0 means no limit. Returns 0 on success, -1 on error/none. */
int djvu_iw44_decode_form(djvu_doc *doc, uint32_t form_off, const char *chunk_id,
                          iw_pixmap *pm, int max_chunks);

int djvu_iw44_width(iw_pixmap *pm);
int djvu_iw44_height(iw_pixmap *pm);
int djvu_iw44_is_color(iw_pixmap *pm);

/* Render full-resolution. `rgb` must hold width*height*3 bytes (R,G,B order).
   Gray images are expanded to gray RGB. Returns 0 on success. */
int djvu_iw44_render_rgb(iw_pixmap *pm, uint8_t *rgb);

/* Like render_rgb but bottom-up (row 0 = bottom), matching DjVuLibre's internal
   GPixmap orientation; used by the compositor. */
int djvu_iw44_render_rgb_raw(iw_pixmap *pm, uint8_t *rgb);

/* Render full-resolution gray (1 byte/pixel, 0..255). Returns 0 on success. */
int djvu_iw44_render_gray(iw_pixmap *pm, uint8_t *gray);

/* debug: render plane 0=Y,1=Cb,2=Cr as gray (value+128). */
int djvu_iw44_render_plane(iw_pixmap *pm, int plane, uint8_t *gray);

/* ===================================================================== */
/* scaler.c -- GPixmapScaler bilinear upsampler (compose background path) */
/* ===================================================================== */

typedef struct { int w, h; uint8_t *d; } djvu_cpix;  /* RGB, w*h*3 */

int  djvu_cpix_init(djvu_ctx *ctx, djvu_cpix *p, int w, int h);
void djvu_cpix_free(djvu_ctx *ctx, djvu_cpix *p);
int  djvu_compute_red(int w, int h, int rw, int rh);
int  djvu_cpix_scale(djvu_ctx *ctx, const djvu_cpix *in, djvu_cpix *out,
                     int outw, int outh, int red);
/* Internal; djvu_doc_open calls this (same as public djvu_init). */
void djvu_scaler_init(void);

/* ===================================================================== */
/* compose.c -- page compositing (IW44 bg + JB2 mask + fg)                 */
/* ===================================================================== */

int djvu_compose_background(djvu_doc *doc, uint32_t form_off, int width, int height,
                            djvu_cpix *out);

/* Per-stage render timings (bench -layers); milliseconds. */
typedef struct {
    double jb2_ms;
    double iw44_ms;
    double composite_ms;
    double rotate_ms;
} djvu_render_timings;

static inline void djvu_render_timings_clear(djvu_render_timings *t)
{
    t->jb2_ms = t->iw44_ms = t->composite_ms = t->rotate_ms = 0.0;
}

djvu_image *djvu_compose_page(djvu_doc *doc, int page_no, jb2_image *mask,
                              int width, int height, djvu_render_timings *t);

/* Composite a color page directly into a caller buffer (top-down, dst stride in
   bytes); no intermediate djvu_image. dst must hold width*height*3. */
int djvu_compose_page_into(djvu_doc *doc, int page_no, jb2_image *mask,
                           int width, int height, uint8_t *dst, int stride);

/* Like djvu_page_render; optional per-stage timings when t != NULL. */
djvu_image *djvu_page_render_timed(djvu_doc *doc, int page_no, int subsample,
                                   djvu_render_timings *t);

/* ===================================================================== */
/* debug.c -- test-harness helpers (not part of the public API)           */
/* ===================================================================== */

void djvu_debug_dump_comps(djvu_doc *doc);
djvu_image *djvu_debug_render_iw(djvu_doc *doc, int page_no, int kind);
djvu_image *djvu_debug_render_iw_gray(djvu_doc *doc, int page_no, int kind);
djvu_image *djvu_debug_render_iw_plane(djvu_doc *doc, int page_no, int kind, int plane);
djvu_image *djvu_debug_render_bg(djvu_doc *doc, int page_no);
int djvu_debug_dump_iw(djvu_doc *doc, int page_no, int kind, const char *path);

/* Per-page verify memory snapshot (stderr; test harness only). */
void djvu_debug_verify_mem(djvu_doc *doc, int page_no, const char *stage, FILE *out);

#endif /* DJVU_INTERNAL_H */
