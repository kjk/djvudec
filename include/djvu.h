/* djvu.h -- plain-C DjVu decoder (port of DjvuNet, jbig2dec-flavored API).
 *
 * Decode-only. The caller supplies the entire DjVu file up-front as an
 * in-memory buffer that must outlive the djvu_doc.
 */
#ifndef DJVU_H
#define DJVU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- allocator + diagnostics (jbig2dec style) ----- */

typedef void *(*djvu_alloc_cb)(void *user, size_t size);
typedef void  (*djvu_free_cb)(void *user, void *ptr);

typedef enum {
    DJVU_SEVERITY_DEBUG,
    DJVU_SEVERITY_INFO,
    DJVU_SEVERITY_WARNING,
    DJVU_SEVERITY_ERROR,
    DJVU_SEVERITY_FATAL
} djvu_severity;

/* msg is a NUL-terminated, already-formatted message. */
typedef void (*djvu_error_cb)(void *user, djvu_severity sev, const char *msg);

typedef struct djvu_ctx djvu_ctx;
typedef struct djvu_doc djvu_doc;

/* Pass NULL for alloc/free to use the default malloc/free.
   Pass NULL for error to silently ignore diagnostics. */
djvu_ctx *djvu_ctx_new(djvu_alloc_cb alloc, djvu_free_cb free_cb,
                       djvu_error_cb error, void *user);
void djvu_ctx_free(djvu_ctx *ctx);

/* ----- documents ----- */

/* Open a document over an in-memory buffer (NOT copied; must remain valid
   until djvu_doc_close). Returns NULL on failure (diagnostics via error cb). */
djvu_doc *djvu_doc_open(djvu_ctx *ctx, const uint8_t *data, size_t len);
void djvu_doc_close(djvu_doc *doc);

/* Number of displayable pages (FORM:DJVU components). */
int djvu_doc_page_count(djvu_doc *doc);

typedef struct {
    int width;     /* full-resolution width in pixels  */
    int height;    /* full-resolution height in pixels */
    int dpi;       /* dots per inch */
    int version;   /* djvu minor version from INFO */
    int rotation;  /* 0, 90, 180, 270 (degrees clockwise) */
} djvu_page_info;

/* page_no is 0-based. Returns 0 on success, -1 on error. */
int djvu_doc_page_info(djvu_doc *doc, int page_no, djvu_page_info *info);

/* ----- rendering ----- */

typedef enum {
    DJVU_FORMAT_GRAY8 = 1,  /* 1 byte/pixel, 0=black .. 255=white */
    DJVU_FORMAT_RGB24 = 3   /* 3 bytes/pixel, R,G,B */
} djvu_format;

typedef struct {
    int width;
    int height;
    djvu_format format;
    int stride;        /* bytes per row */
    uint8_t *data;     /* width*height*components, top-down */
} djvu_image;

/* Render the full composite page image.
   subsample >= 1 reduces resolution by that integer factor (1 = full res).
   Returns NULL on error; free with djvu_image_destroy. */
djvu_image *djvu_page_render(djvu_doc *doc, int page_no, int subsample);
void djvu_image_destroy(djvu_ctx *ctx, djvu_image *img);

/* ----- text ----- */

/* Returns malloc'd (via ctx allocator) NUL-terminated UTF-8 page text,
   or NULL if the page has no text. Free with djvu_text_destroy. */
char *djvu_page_text(djvu_doc *doc, int page_no);
void djvu_text_destroy(djvu_ctx *ctx, char *text);

#ifdef __cplusplus
}
#endif
#endif /* DJVU_H */
