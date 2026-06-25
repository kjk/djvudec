/* bufread.c -- bounded-buffer string reads (NAVM outline strings, etc.). */
#include "djvu_internal.h"
#include <string.h>

char *djvu_br_strdup(djvu_buf_reader *br, int slen)
{
    char *s;
    if (slen < 0 || br->pos + (size_t)slen > br->len) { br->failed = 1; return NULL; }
    s = (char *)djvu_alloc(br->ctx, (size_t)slen + 1);
    if (!s) { br->failed = 1; return NULL; }
    memcpy(s, br->p + br->pos, (size_t)slen);
    s[slen] = 0;
    br->pos += (size_t)slen;
    return s;
}