/* oversize_iw44.c -- regression for the IW44 bw*bh / nb integer overflow.
 *
 * A crafted BG44 chunk declares a 65535x65535 IW44 background. map_new padded
 * the dimensions to 65536x65536 and computed the block count as
 *   nb = (bw * bh) / 1024
 * in int, so bw*bh = 2^32 wrapped to 0 (signed overflow, UB): m->blocks was
 * allocated empty while build_unified still walked the real 2048x2048 macro
 * blocks -> heap OOB, and the render path first tried to allocate the real
 * ~4-12 GB coeff/RGB buffers.
 *
 * The bug only bites once a multi-gigabyte allocation succeeds, so it is not a
 * deterministic crash. Instead we render under a budgeted allocator that
 * refuses anything over a modest cap and records the largest size ever
 * requested. The fix rejects the oversize dimensions in map_new, so the
 * background decode fails and the giant buffers are never requested:
 *   - fixed:   largest request stays small -> PASS
 *   - pre-fix: render asks for gigabytes   -> FAIL
 *
 * The page INFO dimensions must equal the BG44 native size, else the
 * compose reduction factor is 0 and the background is skipped before it
 * allocates. Self-contained: builds the .djvu in memory, no fixture file.
 *
 * To keep the test from passing vacuously (a crafted file that no longer opens,
 * or a render that bails out before it reaches the IW44 decoder, would also
 * "request nothing large"), it additionally requires that the document opens
 * as one 65535x65535 photo page and that each render actually allocated
 * something -- a photo render's first allocation is the IW44 decoder state, so
 * a non-zero count means the BG44 decode was entered -- before failing. */
#include "djvu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Refuse any single allocation over this. With the fix the oversize background
   is rejected at decode and nothing large is requested; pre-fix the native
   coeff/RGB buffers are gigabytes, so they trip it immediately. */
#define ALLOC_CAP ((size_t)256 * 1024 * 1024)

static size_t g_max_req; /* largest size ever passed to alloc, cap or not */
static long g_nalloc;    /* number of alloc calls (reset around each render) */

static void *cap_alloc(void *user, void *ctx, size_t size)
{
    (void)user; (void)ctx;
    g_nalloc++;
    if (size > g_max_req) g_max_req = size;
    if (size > ALLOC_CAP) return NULL; /* exercise the alloc-failure paths */
    return malloc(size ? size : 1);
}

static void cap_free(void *user, void *ctx, void *ptr)
{
    (void)user; (void)ctx;
    free(ptr);
}

/* ---- build a single-page FORM:DJVU with INFO + an oversize BG44 ---- */

static void put_u32be(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static size_t put_chunk(unsigned char *out, const char *id,
                        const unsigned char *body, size_t n)
{
    size_t k = 0;
    memcpy(out + k, id, 4); k += 4;
    put_u32be(out + k, (unsigned)n); k += 4;
    memcpy(out + k, body, n); k += n;
    if (n & 1) out[k++] = 0; /* IFF even-pad */
    return k;
}

static size_t build_djvu(unsigned char *out)
{
    /* INFO: width/height u16be, minor,major u8, dpi u16le, gamma, flags.
       Dims match the BG44 native size so compose_background's reduction
       factor is 1 and it proceeds to allocate the native buffer. */
    const unsigned char info[] = {
        0xff, 0xff, 0xff, 0xff, 24, 0, 0x2c, 0x01, 22, 0,
    };
    /* BG44 IW44 primary chunk: serial=0, slices=1, major=1, minor=1,
       w=0xffff, h=0xffff, then two ZP bytes. minor<2 -> no crcbdelay byte. */
    const unsigned char bg44[] = {
        0, 1, 1, 1, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    };

    unsigned char djvu[256];
    size_t d = 0;
    memcpy(djvu + d, "DJVU", 4); d += 4;
    d += put_chunk(djvu + d, "INFO", info, sizeof(info));
    d += put_chunk(djvu + d, "BG44", bg44, sizeof(bg44));

    size_t k = 0;
    memcpy(out + k, "AT&T", 4); k += 4;
    memcpy(out + k, "FORM", 4); k += 4;
    put_u32be(out + k, (unsigned)(4 + d)); k += 4; /* "DJVU" + chunks */
    memcpy(out + k, djvu, d); k += d;
    return k;
}

/* Render the page at `subsample`; the oversize background must be rejected
   (NULL) after the IW44 decode was entered (>= 1 allocation). Returns 0 if so. */
static int render_must_reject(djvu_ctx *ctx, djvu_doc *doc, int subsample)
{
    djvu_image *img;
    long nalloc;

    g_nalloc = 0;
    img = djvu_page_render(doc, 0, subsample);
    nalloc = g_nalloc;
    if (img) {
        fprintf(stderr, "oversize_iw44: FAIL -- render at subsample %d returned a "
                        "%dx%d image; the oversize BG44 was not rejected\n",
                subsample, img->width, img->height);
        djvu_image_destroy(ctx, img);
        return 1;
    }
    if (nalloc == 0) {
        fprintf(stderr, "oversize_iw44: FAIL -- render at subsample %d failed "
                        "without allocating; the IW44 decode was never reached\n",
                subsample);
        return 1;
    }
    return 0;
}

int main(void)
{
    unsigned char file[512];
    size_t len = build_djvu(file);
    djvu_ctx *ctx;
    djvu_doc *doc;
    djvu_page_info pi;
    int rc = 0;

    djvu_init();
    ctx = djvu_ctx_new(cap_alloc, cap_free, NULL, NULL, NULL, NULL);
    if (!ctx) { fprintf(stderr, "oversize_iw44: ctx_new failed\n"); return 2; }

    doc = djvu_doc_open(ctx, file, len);
    if (!doc) {
        fprintf(stderr, "oversize_iw44: FAIL -- crafted document did not open; "
                        "the test no longer reaches the IW44 decoder\n");
        djvu_ctx_free(ctx);
        return 1;
    }
    memset(&pi, 0, sizeof(pi));
    if (djvu_doc_page_count(doc) != 1 || djvu_doc_page_info(doc, 0, &pi) != 0 ||
        pi.width != 65535 || pi.height != 65535 ||
        djvu_page_get_type(doc, 0) != DJVU_PAGE_PHOTO) {
        fprintf(stderr, "oversize_iw44: FAIL -- expected one 65535x65535 photo "
                        "page, got %d page(s), %dx%d, type %d\n",
                djvu_doc_page_count(doc), pi.width, pi.height,
                (int)djvu_page_get_type(doc, 0));
        rc = 1;
    } else {
        rc |= render_must_reject(ctx, doc, 1);
        rc |= render_must_reject(ctx, doc, 4);
    }
    djvu_doc_close(doc);
    djvu_ctx_free(ctx);

    if (g_max_req > ALLOC_CAP) {
        fprintf(stderr,
                "oversize_iw44: FAIL -- render requested %zu bytes "
                "(> cap %zu); oversize IW44 dims not rejected\n",
                g_max_req, (size_t)ALLOC_CAP);
        rc = 1;
    } else if (rc == 0) {
        printf("oversize_iw44: ok (largest request %zu bytes)\n", g_max_req);
    }
    return rc;
}
