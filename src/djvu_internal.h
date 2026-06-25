/* djvu_internal.h -- internal structures and helpers. */
#ifndef DJVU_INTERNAL_H
#define DJVU_INTERNAL_H

#include "djvu.h"
#include <stdarg.h>

struct djvu_ctx {
    djvu_alloc_cb alloc;
    djvu_free_cb  free;
    djvu_error_cb error;
    void *user;
};

void *djvu_alloc(djvu_ctx *ctx, size_t size);
void  djvu_free(djvu_ctx *ctx, void *ptr);
void  djvu_errorf(djvu_ctx *ctx, djvu_severity sev, const char *fmt, ...);

/* A component listed in the DJVM directory (DIRM). */
typedef struct {
    uint32_t offset;     /* file offset of the component's "FORM" tag */
    uint32_t size;       /* component size from DIRM (0 if unknown) */
    char    *id;         /* component id used by INCL refs (NUL-term, owned) */
    int      type;       /* 0=include(DJVI) 1=page 2=thumb 3=shared-anno */
} djvu_component;

/* A displayable page = a FORM:DJVU component in the document. */
typedef struct {
    uint32_t form_off;   /* file offset of the "FORM" tag */
    uint32_t form_size;  /* size field of that FORM chunk */
    int has_info;
    djvu_page_info info;
} djvu_page_int;

struct djvu_doc {
    djvu_ctx *ctx;
    const uint8_t *data;
    size_t len;
    int npages;
    djvu_page_int *pages;
    int ncomp;
    djvu_component *comps;   /* all DIRM components, in directory order */
};

/* Resolve an INCL component id to its FORM file offset; 0 if not found. */
uint32_t djvu_doc_component_offset(djvu_doc *doc, const char *id);

/* Find the first sub-chunk with id `id` inside the FORM at form_off.
   Returns a pointer to the chunk data and sets *out_size, or NULL.
   If `start` != NULL, *start is used as the byte offset to begin scanning and
   is advanced past the found chunk (to iterate repeated chunks like INCL). */
const uint8_t *djvu_form_find_chunk(djvu_doc *doc, uint32_t form_off,
                                    const char *id, uint32_t *out_size,
                                    uint32_t *start);

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

#endif /* DJVU_INTERNAL_H */
