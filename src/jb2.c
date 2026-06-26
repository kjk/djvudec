/* jb2.c -- JB2 bitonal decoder. Ported from DjvuNet JB2/{JB2Codec,JB2Decoder,
 * JB2Dictionary,JB2Image}.cs (decode path only). */
#include "djvu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* record types */
enum {
    REC_StartOfData = 0,
    REC_NewMark = 1,
    REC_NewMarkLibraryOnly = 2,
    REC_NewMarkImageOnly = 3,
    REC_MatchedRefine = 4,
    REC_MatchedRefineLibraryOnly = 5,
    REC_MatchedRefineImageOnly = 6,
    REC_MatchedCopy = 7,
    REC_NonMarkData = 8,
    REC_RequiredDictOrReset = 9,
    REC_PreservedComment = 10,
    REC_EndOfData = 11
};

#define BIGPOSITIVE  262142
#define BIGNEGATIVE (-262143)

typedef struct {
    djvu_ctx *ctx;
    djvu_zp zp;

    /* number-coder tree (parallel arrays; index 0 reserved) */
    uint8_t *bitcells;
    int *leftcell;
    int *rightcell;
    int ncells, cap_cells;

    /* distribution roots (tree indices, init 0) */
    int dist_record_type, dist_match_index;
    int abs_loc_x, abs_loc_y, abs_size_x, abs_size_y;
    int image_size_dist, inherited_shape_count_dist;
    int rel_loc_x_cur, rel_loc_x_last, rel_loc_y_cur, rel_loc_y_last;
    int rel_size_x, rel_size_y;
    int dist_comment_byte, dist_comment_length;
    /* single-bit contexts */
    uint8_t offset_type_dist;
    uint8_t dist_refinement_flag;

    /* bitmap ZP context arrays */
    uint8_t bitdist[1024];
    uint8_t cbitdist[2048];

    /* library mappings */
    int *lib2shape; int nlib2shape, cap_lib2shape;
    int *shape2lib; int nshape2lib, cap_shape2lib;
    int *libinfo;   /* 4 ints/entry: xmin,ymin,xmax,ymax */
    int nlibinfo, cap_libinfo;

    int shortlist[3], shortlistpos;
    int last_left, last_right, last_bottom, last_row_left, last_row_bottom;
    int image_columns, image_rows;
    int got_start_record;
    int refinementp;

    jb2_image *zdict;
    int error;
} jb2_codec;

/* ---------- jb2_image (dictionary + image) ---------- */

jb2_image *jb2_image_new(djvu_ctx *ctx)
{
    jb2_image *im = (jb2_image *)djvu_alloc(ctx, sizeof(jb2_image));
    if (!im) return NULL;
    memset(im, 0, sizeof(*im));
    im->ctx = ctx;
    return im;
}

void djvu_jb2_free(djvu_ctx *ctx, jb2_image *im)
{
    int i;
    if (!im) return;
    for (i = 0; i < im->nshapes; i++)
        djvu_bm_free(ctx, &im->shapes[i].bm);
    djvu_free(ctx, im->shapes);
    djvu_free(ctx, im->blits);
    djvu_free(ctx, im);
}

static int img_shapecount(jb2_image *im)
{
    return im->inherited_shapes + im->nshapes;
}

jb2_shape *djvu_jb2_get_shape(jb2_image *im, int n)
{
    if (n >= im->inherited_shapes)
        return &im->shapes[n - im->inherited_shapes];
    if (im->inherited_dict)
        return djvu_jb2_get_shape(im->inherited_dict, n);
    return NULL;
}

/* add a default-initialized shape, return its shape number (or -1) */
static int img_add_shape(jb2_image *im, int parent)
{
    int no = im->inherited_shapes + im->nshapes;
    if (im->nshapes >= im->cap_shapes) {
        int nc = im->cap_shapes ? im->cap_shapes * 2 : 64;
        jb2_shape *ns = (jb2_shape *)djvu_alloc(im->ctx, sizeof(jb2_shape) * nc);
        if (!ns) return -1;
        if (im->shapes) { memcpy(ns, im->shapes, sizeof(jb2_shape) * im->nshapes);
                          djvu_free(im->ctx, im->shapes); }
        im->shapes = ns; im->cap_shapes = nc;
    }
    memset(&im->shapes[im->nshapes], 0, sizeof(jb2_shape));
    im->shapes[im->nshapes].parent = parent;
    im->nshapes++;
    return no;
}

static int img_add_blit(jb2_image *im, jb2_blit b)
{
    if (im->nblits >= im->cap_blits) {
        int nc = im->cap_blits ? im->cap_blits * 2 : 256;
        jb2_blit *nb = (jb2_blit *)djvu_alloc(im->ctx, sizeof(jb2_blit) * nc);
        if (!nb) return -1;
        if (im->blits) { memcpy(nb, im->blits, sizeof(jb2_blit) * im->nblits);
                         djvu_free(im->ctx, im->blits); }
        im->blits = nb; im->cap_blits = nc;
    }
    im->blits[im->nblits++] = b;
    return 0;
}

static void img_set_inherited_dict(jb2_image *im, jb2_image *dict)
{
    im->inherited_dict = dict;
    im->inherited_shapes = dict ? img_shapecount(dict) : 0;
}

/* ---------- small dynamic int arrays ---------- */

static int iarr_push(djvu_ctx *ctx, int **arr, int *n, int *cap, int v)
{
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        int *na = (int *)djvu_alloc(ctx, sizeof(int) * nc);
        if (!na) return -1;
        if (*arr) { memcpy(na, *arr, sizeof(int) * *n); djvu_free(ctx, *arr); }
        *arr = na; *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 0;
}

/* ---------- ZP bit coding ---------- */

static int code_bit_cell(jb2_codec *c, int cell)
{
    return djvu_zp_decode(&c->zp, &c->bitcells[cell]);
}
static void ensure_cells(jb2_codec *c, int need)
{
    if (need <= c->cap_cells) return;
    {
        int nc = c->cap_cells ? c->cap_cells : 256;
        uint8_t *nb; int *nl, *nr;
        while (nc < need) nc *= 2;
        nb = (uint8_t *)djvu_alloc(c->ctx, sizeof(uint8_t) * nc);
        nl = (int *)djvu_alloc(c->ctx, sizeof(int) * nc);
        nr = (int *)djvu_alloc(c->ctx, sizeof(int) * nc);
        if (!nb || !nl || !nr) { c->error = 1; djvu_free(c->ctx, nb);
            djvu_free(c->ctx, nl); djvu_free(c->ctx, nr); return; }
        if (c->ncells) {
            memcpy(nb, c->bitcells, sizeof(uint8_t) * c->ncells);
            memcpy(nl, c->leftcell, sizeof(int) * c->ncells);
            memcpy(nr, c->rightcell, sizeof(int) * c->ncells);
        }
        djvu_free(c->ctx, c->bitcells); djvu_free(c->ctx, c->leftcell);
        djvu_free(c->ctx, c->rightcell);
        c->bitcells = nb; c->leftcell = nl; c->rightcell = nr;
        c->cap_cells = nc;
    }
}

/* CodeNum (decode). ctxslot points to a distribution root int. */
static int code_num(jb2_codec *c, int low, int high, int *ctxslot)
{
    int negative = 0, cutoff = 0, phase = 1, range = -1;
    int *ctx = ctxslot;

    ensure_cells(c, c->ncells + 80);
    if (c->error) return 0;

    while (range != 1) {
        int ictx = *ctx;
        int decision;
        if (ictx == 0) {
            ictx = c->ncells;
            *ctx = ictx;
            c->bitcells[ictx] = 0;
            c->leftcell[ictx] = 0;
            c->rightcell[ictx] = 0;
            c->ncells++;
        }
        decision = (low >= cutoff) ||
                   ((high >= cutoff) && code_bit_cell(c, ictx) != 0);
        ctx = decision ? &c->rightcell[ictx] : &c->leftcell[ictx];

        switch (phase) {
        case 1:
            negative = !decision;
            if (negative) { int t = -low - 1; low = -high - 1; high = t; }
            phase = 2; cutoff = 1;
            break;
        case 2:
            if (!decision) {
                phase = 3;
                range = (cutoff + 1) >> 1;
                if (range == 1) cutoff = 0;
                else cutoff -= (range >> 1);
            } else {
                cutoff = (cutoff << 1) + 1;
            }
            break;
        case 3:
            range /= 2;
            if (range != 1) {
                if (!decision) cutoff -= (range >> 1);
                else cutoff += (range >> 1);
            } else if (!decision) {
                cutoff--;
            }
            break;
        }
    }
    return negative ? (-cutoff - 1) : cutoff;
}

/* ---------- bitmap decoders ---------- */

/* Decode one JB2 bitmap pixel; hoisted a/fence stay in registers across the row. */
static inline int jb2_zp_decode_pixel(djvu_zp *zp, uint32_t *a, uint32_t *fence,
                                      uint8_t *ctx)
{
    uint32_t z = *a + zp->p[*ctx];
    if (z <= *fence) {
        *a = z;
        return *ctx & 1;
    }
    zp->a = *a;
    z = zp_decode_sub(zp, ctx, z);
    *a = zp->a;
    *fence = zp->fence;
    return z;
}

static void code_bitmap_directly(jb2_codec *c, djvu_bitmap *bm)
{
    int dw, dy, bpr, h;
    uint8_t *row_base, *guard, *up2, *up1, *up0;
    djvu_zp *zp;
    uint8_t *bd;
    uint32_t a, fence;

    djvu_bm_set_min_border(c->ctx, bm, 3);
    dw = bm->width;
    h = bm->height;
    dy = h - 1;
    bpr = bm->bytes_per_row;
    row_base = bm->data + bm->border;
    guard = bm->guard + bm->border;
    zp = &c->zp;
    bd = c->bitdist;
    a = zp->a;
    fence = zp->fence;
    up2 = (dy + 2 < h) ? row_base + (dy + 2) * bpr : guard;
    up1 = (dy + 1 < h) ? row_base + (dy + 1) * bpr : guard;
    up0 = row_base + dy * bpr;
    while (dy >= 0) {
        int context = jb2_get_direct_context(up2, up1, up0, 0);
        int dx = 0;
        while (dx < dw) {
            int n = jb2_zp_decode_pixel(zp, &a, &fence, &bd[context]);
            up0[dx++] = (uint8_t)n;
            context = jb2_shift_direct_context(context, n, up2, up1, dx);
        }
        dy--;
        up2 = up1;
        up1 = up0;
        up0 = (dy >= 0) ? row_base + dy * bpr : guard;
    }
    zp->a = a;
}

static void code_bitmap_cross(jb2_codec *c, djvu_bitmap *bm, djvu_bitmap *cbm, int libno)
{
    int cw = cbm->width, dw = bm->width, dh = bm->height, ch = cbm->height;
    int xmin = c->libinfo[libno * 4 + 0];
    int xmax = c->libinfo[libno * 4 + 2];
    int ymin = c->libinfo[libno * 4 + 1];
    int ymax = c->libinfo[libno * 4 + 3];
    int xd2c = ((1 + (dw >> 1)) - dw) - ((((1 + xmax) - xmin) >> 1) - xmax);
    int yd2c = ((1 + (dh >> 1)) - dh) - ((((1 + ymax) - ymin) >> 1) - ymax);
    int dy, cy, bm_bpr, cbm_bpr;
    uint8_t *bm_base, *cbm_base, *bm_guard, *cbm_guard;
    uint8_t *up1, *up0, *xup1, *xup0, *xdn1;
    djvu_zp *zp;
    uint8_t *bd;
    uint32_t a, fence;

    djvu_bm_set_min_border(c->ctx, bm, 2);
    djvu_bm_set_min_border(c->ctx, cbm, 2 - xd2c);
    djvu_bm_set_min_border(c->ctx, cbm, (2 + dw + xd2c) - cw);

    bm_bpr = bm->bytes_per_row;
    cbm_bpr = cbm->bytes_per_row;
    bm_base = bm->data + bm->border;
    cbm_base = cbm->data + cbm->border;
    bm_guard = bm->guard + bm->border;
    cbm_guard = cbm->guard + cbm->border;
    zp = &c->zp;
    bd = c->cbitdist;
    a = zp->a;
    fence = zp->fence;
    dy = dh - 1;
    cy = dy + yd2c;
    up1 = (dy + 1 < dh) ? bm_base + (dy + 1) * bm_bpr : bm_guard;
    up0 = bm_base + dy * bm_bpr;
    /* cy = dy + yd2c can fall outside [0, ch) on EITHER side when the reference
       shape differs in size, so every cross-bitmap row needs both bounds checked
       (rows out of range read the zero guard). The bm bitmap above only needs the
       upper check because its dy stays within [0, dh). */
    xup1 = (cy + 1 >= 0 && cy + 1 < ch) ? cbm_base + (cy + 1) * cbm_bpr + xd2c : cbm_guard + xd2c;
    xup0 = (cy >= 0 && cy < ch) ? cbm_base + cy * cbm_bpr + xd2c : cbm_guard + xd2c;
    xdn1 = (cy - 1 >= 0 && cy - 1 < ch) ? cbm_base + (cy - 1) * cbm_bpr + xd2c : cbm_guard + xd2c;

    while (dy >= 0) {
        int context = jb2_get_cross_context(up1, up0, xup1, xup0, xdn1, 0);
        int dx = 0;
        while (dx < dw) {
            int n = jb2_zp_decode_pixel(zp, &a, &fence, &bd[context]);
            up0[dx++] = (uint8_t)n;
            context = jb2_shift_cross_context(context, n, up1, xup1, xup0, xdn1, dx);
        }
        dy--;
        up1 = up0;
        up0 = (dy >= 0) ? bm_base + dy * bm_bpr : bm_guard;
        cy--;
        xup1 = xup0;
        xup0 = xdn1;
        xdn1 = (cy - 1 >= 0 && cy - 1 < ch) ? cbm_base + (cy - 1) * cbm_bpr + xd2c : cbm_guard + xd2c;
    }
    zp->a = a;
}

/* ---------- library bookkeeping ---------- */

static void shape2lib_set(jb2_codec *c, int shapeno, int libno)
{
    while (c->nshape2lib <= shapeno)
        iarr_push(c->ctx, &c->shape2lib, &c->nshape2lib, &c->cap_shape2lib, -1);
    c->shape2lib[shapeno] = libno;
}

static int add_library(jb2_codec *c, int shapeno, jb2_shape *jshp)
{
    int libno = c->nlib2shape;
    int xmin, ymin, xmax, ymax;
    iarr_push(c->ctx, &c->lib2shape, &c->nlib2shape, &c->cap_lib2shape, shapeno);
    shape2lib_set(c, shapeno, libno);
    djvu_bm_bbox(&jshp->bm, &xmin, &ymin, &xmax, &ymax);
    iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, xmin);
    iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, ymin);
    iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, xmax);
    iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, ymax);
    return libno;
}

static void init_library(jb2_codec *c, jb2_image *jim)
{
    int nshape = jim->inherited_shapes;
    int i;
    c->nshape2lib = c->nlib2shape = c->nlibinfo = 0;
    for (i = 0; i < nshape; i++) {
        jb2_shape *jshp = djvu_jb2_get_shape(jim, i);
        int xmin, ymin, xmax, ymax;
        iarr_push(c->ctx, &c->shape2lib, &c->nshape2lib, &c->cap_shape2lib, i);
        iarr_push(c->ctx, &c->lib2shape, &c->nlib2shape, &c->cap_lib2shape, i);
        djvu_bm_bbox(&jshp->bm, &xmin, &ymin, &xmax, &ymax);
        iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, xmin);
        iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, ymin);
        iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, xmax);
        iarr_push(c->ctx, &c->libinfo, &c->nlibinfo, &c->cap_libinfo, ymax);
    }
}

/* ---------- short list (row bottom tracking) ---------- */

/* ResetNumcoder: clear all number-coder distributions and the bitcell tree.
   Emitted periodically by the encoder (RequiredDictOrReset) to bound memory. */
static void reset_numcoder(jb2_codec *c)
{
    c->dist_comment_byte = 0; c->dist_comment_length = 0;
    c->dist_record_type = 0; c->dist_match_index = 0;
    c->abs_loc_x = 0; c->abs_loc_y = 0; c->abs_size_x = 0; c->abs_size_y = 0;
    c->image_size_dist = 0; c->inherited_shape_count_dist = 0;
    c->rel_loc_x_cur = 0; c->rel_loc_x_last = 0;
    c->rel_loc_y_cur = 0; c->rel_loc_y_last = 0;
    c->rel_size_x = 0; c->rel_size_y = 0;
    c->ncells = 1;   /* drop the tree; index 0 stays reserved */
}

static void fill_shortlist(jb2_codec *c, int v)
{
    c->shortlist[0] = c->shortlist[1] = c->shortlist[2] = v;
    c->shortlistpos = 0;
}

static int update_shortlist(jb2_codec *c, int v)
{
    int *s = c->shortlist;
    if (++c->shortlistpos == 3) c->shortlistpos = 0;
    s[c->shortlistpos] = v;
    return (s[0] >= s[1])
        ? ((s[0] > s[2]) ? ((s[1] >= s[2]) ? s[1] : s[2]) : s[0])
        : ((s[0] < s[2]) ? ((s[1] >= s[2]) ? s[2] : s[1]) : s[0]);
}

/* ---------- coding primitives ---------- */

static int code_record_type(jb2_codec *c)
{
    return code_num(c, REC_StartOfData, REC_EndOfData, &c->dist_record_type);
}

static int code_match_index(jb2_codec *c)
{
    return code_num(c, 0, c->nlib2shape - 1, &c->dist_match_index);
}

static int code_abs_mark_size(jb2_codec *c, djvu_bitmap *bm, int border)
{
    int xsize = code_num(c, 0, BIGPOSITIVE, &c->abs_size_x);
    int ysize = code_num(c, 0, BIGPOSITIVE, &c->abs_size_y);
    if (xsize != (0xffff & xsize) || ysize != (0xffff & ysize)) { c->error = 1; return -1; }
    return djvu_bm_init(c->ctx, bm, ysize, xsize, border);
}

static int code_rel_mark_size(jb2_codec *c, djvu_bitmap *bm, int cw, int ch, int border)
{
    int xdiff = code_num(c, BIGNEGATIVE, BIGPOSITIVE, &c->rel_size_x);
    int ydiff = code_num(c, BIGNEGATIVE, BIGPOSITIVE, &c->rel_size_y);
    int xsize = cw + xdiff, ysize = ch + ydiff;
    if (xsize != (0xffff & xsize) || ysize != (0xffff & ysize)) { c->error = 1; return -1; }
    return djvu_bm_init(c->ctx, bm, ysize, xsize, border);
}

static void code_image_size_image(jb2_codec *c, jb2_image *jim)
{
    c->image_columns = code_num(c, 0, BIGPOSITIVE, &c->image_size_dist);
    c->image_rows = code_num(c, 0, BIGPOSITIVE, &c->image_size_dist);
    if (c->image_columns == 0 || c->image_rows == 0) { c->error = 1; return; }
    jim->width = c->image_columns;
    jim->height = c->image_rows;
    /* base.CodeImageSize(JB2Image) */
    c->last_left = 1 + c->image_columns;
    c->last_row_bottom = c->image_rows;
    c->last_row_left = c->last_right = 0;
    fill_shortlist(c, c->last_row_bottom);
    c->got_start_record = 1;
}

static void code_image_size_dict(jb2_codec *c)
{
    int w = code_num(c, 0, BIGPOSITIVE, &c->image_size_dist);
    int h = code_num(c, 0, BIGPOSITIVE, &c->image_size_dist);
    if (w != 0 || h != 0) { c->error = 1; return; }
    /* base.CodeImageSize(JB2Dictionary) */
    c->last_left = 1;
    c->last_row_bottom = 0;
    c->last_row_left = c->last_right = 0;
    fill_shortlist(c, c->last_row_bottom);
    c->got_start_record = 1;
}

static void code_eventual_refinement(jb2_codec *c)
{
    c->refinementp = djvu_zp_decode(&c->zp, &c->dist_refinement_flag);
}

static void code_inherited_shape_count(jb2_codec *c, jb2_image *jim)
{
    int size = code_num(c, 0, BIGPOSITIVE, &c->inherited_shape_count_dist);
    if (jim->inherited_dict == NULL && size > 0) {
        if (c->zdict) img_set_inherited_dict(jim, c->zdict);
        else { c->error = 1; return; }
    }
    if (jim->inherited_dict && size != img_shapecount(jim->inherited_dict))
        c->error = 1;
}

static void code_comment(jb2_codec *c)
{
    int size = code_num(c, 0, BIGPOSITIVE, &c->dist_comment_length);
    int i;
    for (i = 0; i < size; i++)
        (void)code_num(c, 0, 255, &c->dist_comment_byte);
}

static void code_abs_location(jb2_codec *c, jb2_blit *jblt, int rows)
{
    int left = code_num(c, 1, c->image_columns, &c->abs_loc_x);
    int top = code_num(c, 1, c->image_rows, &c->abs_loc_y);
    jblt->bottom = top - rows;
    jblt->left = left - 1;
}

static void code_rel_location(jb2_codec *c, jb2_blit *jblt, int rows, int columns)
{
    int bottom = 0, left = 0, top = 0, right = 0;
    int new_row = djvu_zp_decode(&c->zp, &c->offset_type_dist);
    if (new_row) {
        int x_diff = code_num(c, BIGNEGATIVE, BIGPOSITIVE, &c->rel_loc_x_last);
        int y_diff = code_num(c, BIGNEGATIVE, BIGPOSITIVE, &c->rel_loc_y_last);
        left = c->last_row_left + x_diff;
        top = c->last_row_bottom + y_diff;
        right = (left + columns) - 1;
        bottom = (top - rows) + 1;
        c->last_left = c->last_row_left = left;
        c->last_right = right;
        c->last_bottom = c->last_row_bottom = bottom;
        fill_shortlist(c, bottom);
    } else {
        int x_diff = code_num(c, BIGNEGATIVE, BIGPOSITIVE, &c->rel_loc_x_cur);
        int y_diff = code_num(c, BIGNEGATIVE, BIGPOSITIVE, &c->rel_loc_y_cur);
        left = c->last_right + x_diff;
        bottom = c->last_bottom + y_diff;
        right = (left + columns) - 1;
        top = (bottom + rows) - 1;
        (void)top;
        c->last_left = left;
        c->last_right = right;
        c->last_bottom = update_shortlist(c, bottom);
    }
    jblt->bottom = bottom - 1;
    jblt->left = left - 1;
}

/* ---------- record loop ---------- */

/* Decode one record into jim. jim_is_image selects image vs dict semantics.
   Returns the record type. */
static int code_record(jb2_codec *c, jb2_image *jim, int jim_is_image)
{
    int rectype = code_record_type(c);
    int shape_parent_init = -1;
    int have_shape = 0, have_blit = 0;
    int need_add_library = 0, need_add_blit = 0;
    jb2_shape tmp_shape;     /* working shape (bitmap) before insertion */
    jb2_blit blit;
    int match = -1;
    int parent = -1;

    memset(&tmp_shape, 0, sizeof(tmp_shape));
    memset(&blit, 0, sizeof(blit));

    /* allocate working shape bitmap for record types that need one */
    switch (rectype) {
    case REC_NewMark:
    case REC_NewMarkImageOnly:
    case REC_MatchedRefine:
    case REC_MatchedRefineImageOnly:
    case REC_NonMarkData:
        have_blit = 1; /* fallthrough */
    case REC_NewMarkLibraryOnly:
    case REC_MatchedRefineLibraryOnly:
        shape_parent_init = (rectype == REC_NonMarkData) ? -2 : -1;
        tmp_shape.parent = shape_parent_init;
        have_shape = 1;
        break;
    case REC_MatchedCopy:
        have_blit = 1;
        break;
    }

    switch (rectype) {
    case REC_StartOfData:
        if (jim_is_image) code_image_size_image(c, jim);
        else code_image_size_dict(c);
        code_eventual_refinement(c);
        init_library(c, jim);
        break;

    case REC_NewMark:
        need_add_blit = need_add_library = 1;
        code_abs_mark_size(c, &tmp_shape.bm, 4);
        code_bitmap_directly(c, &tmp_shape.bm);
        code_rel_location(c, &blit, tmp_shape.bm.height, tmp_shape.bm.width);
        break;

    case REC_NewMarkLibraryOnly:
        need_add_library = 1;
        code_abs_mark_size(c, &tmp_shape.bm, 4);
        code_bitmap_directly(c, &tmp_shape.bm);
        break;

    case REC_NewMarkImageOnly:
        need_add_blit = 1;
        code_abs_mark_size(c, &tmp_shape.bm, 3);
        code_bitmap_directly(c, &tmp_shape.bm);
        code_rel_location(c, &blit, tmp_shape.bm.height, tmp_shape.bm.width);
        break;

    case REC_MatchedRefine:
    case REC_MatchedRefineLibraryOnly:
    case REC_MatchedRefineImageOnly: {
        djvu_bitmap *cbm;
        int cw, ch;
        if (rectype == REC_MatchedRefine) { need_add_blit = need_add_library = 1; }
        else if (rectype == REC_MatchedRefineLibraryOnly) { need_add_library = 1; }
        else { need_add_blit = 1; }
        match = code_match_index(c);
        parent = c->lib2shape[match];
        tmp_shape.parent = parent;
        cbm = &djvu_jb2_get_shape(jim, parent)->bm;
        djvu_bm_ensure_bytes(c->ctx, cbm);
        cw = (1 + c->libinfo[match * 4 + 2]) - c->libinfo[match * 4 + 0];
        ch = (1 + c->libinfo[match * 4 + 3]) - c->libinfo[match * 4 + 1];
        code_rel_mark_size(c, &tmp_shape.bm, cw, ch, 4);
        code_bitmap_cross(c, &tmp_shape.bm, cbm, match);
        if (rectype != REC_MatchedRefineLibraryOnly)
            code_rel_location(c, &blit, tmp_shape.bm.height, tmp_shape.bm.width);
        break;
    }

    case REC_MatchedCopy: {
        int xmin, ymin, xmax, ymax;
        match = code_match_index(c);
        blit.shapeno = c->lib2shape[match];
        xmin = c->libinfo[match * 4 + 0];
        ymin = c->libinfo[match * 4 + 1];
        xmax = c->libinfo[match * 4 + 2];
        ymax = c->libinfo[match * 4 + 3];
        blit.left += xmin;
        blit.bottom += ymin;
        code_rel_location(c, &blit, (1 + ymax) - ymin, (1 + xmax) - xmin);
        blit.left -= xmin;
        blit.bottom -= ymin;
        break;
    }

    case REC_NonMarkData:
        need_add_blit = 1;
        code_abs_mark_size(c, &tmp_shape.bm, 3);
        code_bitmap_directly(c, &tmp_shape.bm);
        code_abs_location(c, &blit, tmp_shape.bm.height);
        break;

    case REC_PreservedComment:
        code_comment(c);
        break;

    case REC_RequiredDictOrReset:
        if (!c->got_start_record) code_inherited_shape_count(c, jim);
        else reset_numcoder(c);
        break;

    case REC_EndOfData:
        break;

    default:
        c->error = 1;
        break;
    }

    if (c->error) { if (have_shape) djvu_bm_free(c->ctx, &tmp_shape.bm); return rectype; }

    /* commit shape / blit */
    if (have_shape && (rectype == REC_NewMark || rectype == REC_NewMarkLibraryOnly ||
                       rectype == REC_MatchedRefine || rectype == REC_MatchedRefineLibraryOnly ||
                       rectype == REC_NewMarkImageOnly || rectype == REC_MatchedRefineImageOnly ||
                       rectype == REC_NonMarkData)) {
        int shapeno = img_add_shape(jim, tmp_shape.parent);
        if (shapeno < 0) { c->error = 1; djvu_bm_free(c->ctx, &tmp_shape.bm); return rectype; }
        jim->shapes[jim->nshapes - 1].bm = tmp_shape.bm;  /* move bitmap */
        shape2lib_set(c, shapeno, -1);
        if (need_add_library)
            add_library(c, shapeno, &jim->shapes[jim->nshapes - 1]);
        if (need_add_blit) {
            blit.shapeno = shapeno;
            img_add_blit(jim, blit);
        }
    } else if (rectype == REC_MatchedCopy) {
        img_add_blit(jim, blit);
    }
    (void)have_blit;

    return rectype;
}

/* ---------- public entry ---------- */

static void codec_free(jb2_codec *c)
{
    if (!c) return;
    djvu_free(c->ctx, c->bitcells);
    djvu_free(c->ctx, c->leftcell);
    djvu_free(c->ctx, c->rightcell);
    djvu_free(c->ctx, c->lib2shape);
    djvu_free(c->ctx, c->shape2lib);
    djvu_free(c->ctx, c->libinfo);
}

static jb2_image *jb2_decode_into(djvu_ctx *ctx, const uint8_t *data, size_t len,
                                  jb2_image *dict, int is_image)
{
    jb2_codec *c;
    jb2_image *jim = jb2_image_new(ctx);
    int rectype;
    if (!jim) return NULL;

    c = (jb2_codec *)djvu_alloc(ctx, sizeof(jb2_codec));
    if (!c) { djvu_jb2_free(ctx, jim); return NULL; }

    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    c->zdict = dict;
    /* index 0 of cell arrays is reserved (sentinel) */
    ensure_cells(c, 1);
    c->ncells = 1;

    djvu_zp_init(&c->zp, data, len);

    rectype = REC_StartOfData;
    {
        int hist[12]; int k; int dbg = getenv("DJVU_JB2_DEBUG") != NULL;
        for (k = 0; k < 12; k++) hist[k] = 0;
        do {
            rectype = code_record(c, jim, is_image);
            if (rectype >= 0 && rectype < 12) hist[rectype]++;
            if (c->error) break;
        } while (rectype != REC_EndOfData);
        if (dbg) {
            fprintf(stderr, "JB2 rectypes: SOD=%d NM=%d NMlib=%d NMimg=%d MR=%d "
                "MRlib=%d MRimg=%d MC=%d NMD=%d DICT=%d COM=%d EOD=%d shapes=%d blits=%d\n",
                hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],hist[6],hist[7],
                hist[8],hist[9],hist[10],hist[11], jim->nshapes, jim->nblits);
        }
        {
            const char *shs = getenv("DJVU_JB2_SHAPE");
            if (shs) {
                int sn = atoi(shs);
                jb2_shape *sh = djvu_jb2_get_shape(jim, sn);
                if (sh && djvu_bm_has_pixels(&sh->bm)) {
                    int rr, cc, w = sh->bm.width, h = sh->bm.height;
                    djvu_bm_ensure_bytes(ctx, &sh->bm);
                    fprintf(stderr, "SHAPE %d %dx%d:\n", sn, w, h);
                    for (rr = h - 1; rr >= 0; rr--) {  /* top row first */
                        int ro = djvu_bm_rowoffset(&sh->bm, rr);
                        for (cc = 0; cc < w; cc++)
                            fputc(sh->bm.data[ro + cc] ? '#' : '.', stderr);
                        fputc('\n', stderr);
                    }
                }
            }
        }
        if (getenv("DJVU_JB2_BLITS")) {
            int bi;
            for (bi = 0; bi < jim->nblits; bi++) {
                jb2_blit *b = &jim->blits[bi];
                jb2_shape *sh = djvu_jb2_get_shape(jim, b->shapeno);
                unsigned sum = 0; int rr, cc, w = 0, h = 0;
                if (sh && djvu_bm_has_pixels(&sh->bm)) {
                    w = sh->bm.width; h = sh->bm.height;
                    djvu_bm_ensure_bytes(ctx, &sh->bm);
                    for (rr = 0; rr < h; rr++) {
                        int ro = djvu_bm_rowoffset(&sh->bm, rr);
                        for (cc = 0; cc < w; cc++)
                            sum = sum * 31 + (sh->bm.data[ro + cc] ? 1 : 0);
                    }
                }
                fprintf(stderr, "BLIT %d left=%d bottom=%d shape=%d w=%d h=%d sum=%u\n",
                        bi, b->left, b->bottom, b->shapeno, w, h, sum);
            }
        }
    }

    if (c->error || !c->got_start_record) {
        djvu_errorf(ctx, DJVU_SEVERITY_ERROR, "JB2: decode failed");
        codec_free(c);
        djvu_free(ctx, c);
        djvu_jb2_free(ctx, jim);
        return NULL;
    }
    {
        int si;
        for (si = 0; si < jim->nshapes; si++)
            djvu_bm_compress(ctx, &jim->shapes[si].bm);
    }
    codec_free(c);
    djvu_free(ctx, c);
    return jim;
}

jb2_image *djvu_jb2_decode(djvu_ctx *ctx, const uint8_t *data, size_t len,
                           jb2_image *dict)
{
    /* A Djbz dictionary stream has zero image size; an Sjbz has nonzero.
       We always decode as "image" when no dict caller flag is given, but the
       record semantics only differ in CodeImageSize which validates 0 vs !=0.
       Pages pass their decoded dict here; dictionaries are decoded via
       djvu_jb2_decode_dict below. */
    return jb2_decode_into(ctx, data, len, dict, 1);
}

jb2_image *djvu_jb2_decode_dict(djvu_ctx *ctx, const uint8_t *data, size_t len)
{
    return jb2_decode_into(ctx, data, len, NULL, 0);
}
