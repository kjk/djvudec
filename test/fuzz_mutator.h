/* fuzz_mutator.h -- structure-aware libFuzzer mutator for IFF/DjVu files.
 *
 * Byte-level mutation of a .djvu file mostly breaks the container: one
 * inserted byte desyncs every chunk size after it, and every bundled DIRM
 * offset. This mutator parses the input into its IFF tree (AT&T, nested
 * FORMs, leaf chunks), applies one *structural* or *payload* mutation, then
 * re-serializes with correct chunk sizes, even padding and (usually)
 * recomputed DIRM component offsets -- so the mutated bytes reach the codecs
 * instead of dying in the IFF walker. A share of calls still does a plain
 * LLVMFuzzerMutate of the whole buffer (and of inputs that do not parse) so
 * the container parser keeps getting broken-structure inputs too.
 *
 * Mutations: mutate one leaf payload (libFuzzer's byte mutators, sized to the
 * chunk), delete / duplicate / swap nodes, retag a chunk id or FORM type from
 * the known-ids table; crossover grafts a node of one input into another.
 * Include from exactly one fuzz target (defines the libFuzzer hooks). */
#ifndef FUZZ_MUTATOR_H
#define FUZZ_MUTATOR_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

enum { FM_MAX_NODES = 2048, FM_MAX_DEPTH = 8 };

typedef struct {
    char id[4];
    char type[4];        /* FORM type (is_form) */
    int is_form;
    const uint8_t *p;    /* leaf payload (borrowed or owned) */
    size_t n;
    uint8_t *owned;
    int parent, first, next; /* tree links (-1 = none) */
} fm_node;

typedef struct {
    fm_node nodes[FM_MAX_NODES];
    int count;
    int root;
    int att;             /* input started with "AT&T" */
} fm_tree;

static uint32_t fm_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t fm_rand(uint32_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

static int fm_new(fm_tree *t)
{
    fm_node *nd;
    if (t->count >= FM_MAX_NODES) return -1;
    nd = &t->nodes[t->count];
    memset(nd, 0, sizeof(*nd));
    nd->parent = nd->first = nd->next = -1;
    return t->count++;
}

static void fm_append_child(fm_tree *t, int parent, int kid)
{
    int *link = &t->nodes[parent].first;
    while (*link >= 0) link = &t->nodes[*link].next;
    *link = kid;
    t->nodes[kid].parent = parent;
}

/* Parse chunks in [p, p+n) as children of parent. Lenient: an over-long
   size is clamped to what is left. */
static void fm_parse_kids(fm_tree *t, int parent, const uint8_t *p, size_t n, int depth)
{
    size_t pos = 0;
    while (pos + 8 <= n) {
        uint32_t sz = fm_rd32(p + pos + 4);
        int k = fm_new(t);
        fm_node *nd;
        if (k < 0) return;
        nd = &t->nodes[k];
        memcpy(nd->id, p + pos, 4);
        if (sz > n - pos - 8) sz = (uint32_t)(n - pos - 8);
        if (memcmp(nd->id, "FORM", 4) == 0 && sz >= 4 && depth < FM_MAX_DEPTH) {
            nd->is_form = 1;
            memcpy(nd->type, p + pos + 8, 4);
            fm_append_child(t, parent, k);
            fm_parse_kids(t, k, p + pos + 12, sz - 4, depth + 1);
        } else {
            nd->p = p + pos + 8;
            nd->n = sz;
            fm_append_child(t, parent, k);
        }
        pos += 8 + (size_t)sz + (sz & 1);
    }
}

/* Root is a synthetic node whose children are the top-level chunks. */
static int fm_parse(fm_tree *t, const uint8_t *d, size_t n)
{
    t->count = 0;
    t->att = n >= 4 && memcmp(d, "AT&T", 4) == 0;
    if (t->att) { d += 4; n -= 4; }
    if (n < 12 || memcmp(d, "FORM", 4) != 0) return -1;
    t->root = fm_new(t);
    fm_parse_kids(t, t->root, d, n, 0);
    return t->nodes[t->root].first >= 0 ? 0 : -1;
}

static void fm_free(fm_tree *t)
{
    int i;
    for (i = 0; i < t->count; i++) free(t->nodes[i].owned);
}

/* ---- serialization ------------------------------------------------------- */

typedef struct { uint8_t *d; size_t n, max; int over; } fm_out;

static void fm_put(fm_out *o, const void *p, size_t n)
{
    if (o->over || n > o->max - o->n) { o->over = 1; return; }
    memcpy(o->d + o->n, p, n);
    o->n += n;
}

static void fm_put32(fm_out *o, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fm_put(o, b, 4);
}

/* off[] records each node's output offset (of its id), for DIRM fixup. */
static void fm_write_node(const fm_tree *t, int k, fm_out *o, size_t *off)
{
    const fm_node *nd = &t->nodes[k];
    size_t sz_at, len;
    off[k] = o->n;
    fm_put(o, nd->id, 4);
    sz_at = o->n;
    fm_put32(o, 0);
    if (nd->is_form) {
        int c;
        fm_put(o, nd->type, 4);
        for (c = nd->first; c >= 0; c = t->nodes[c].next) fm_write_node(t, c, o, off);
    } else {
        fm_put(o, nd->p, nd->n);
    }
    if (o->over) return;
    len = o->n - sz_at - 4;
    o->d[sz_at] = (uint8_t)(len >> 24); o->d[sz_at + 1] = (uint8_t)(len >> 16);
    o->d[sz_at + 2] = (uint8_t)(len >> 8); o->d[sz_at + 3] = (uint8_t)len;
    if (len & 1) { uint8_t z = 0; fm_put(o, &z, 1); }
}

/* Bundled DIRM: [flags|0x80][u16 count][u32 offset x count]... -- point the
   offsets at the FORM children of the top FORM, in order (written in place
   in the output: the DIRM payload length does not change). */
static void fm_fix_dirm(const fm_tree *t, fm_out *o, const size_t *off)
{
    int top = t->nodes[t->root].first, c, dirm = -1, i = 0, count;
    const uint8_t *dp;
    if (top < 0 || !t->nodes[top].is_form) return;
    for (c = t->nodes[top].first; c >= 0; c = t->nodes[c].next)
        if (!t->nodes[c].is_form && memcmp(t->nodes[c].id, "DIRM", 4) == 0) { dirm = c; break; }
    if (dirm < 0 || t->nodes[dirm].n < 3) return;
    dp = o->d + off[dirm] + 8;
    if (!(dp[0] & 0x80)) return;
    count = (dp[1] << 8) | dp[2];
    for (c = t->nodes[top].first; c >= 0 && i < count; c = t->nodes[c].next) {
        size_t at = off[dirm] + 8 + 3 + 4 * (size_t)i, v;
        if (!t->nodes[c].is_form) continue;
        if (3 + 4 * (size_t)(i + 1) > t->nodes[dirm].n) break;
        v = off[c];
        o->d[at] = (uint8_t)(v >> 24); o->d[at + 1] = (uint8_t)(v >> 16);
        o->d[at + 2] = (uint8_t)(v >> 8); o->d[at + 3] = (uint8_t)v;
        i++;
    }
}

static size_t fm_serialize(const fm_tree *t, uint8_t *d, size_t max, int fix_dirm)
{
    fm_out o = { d, 0, max, 0 };
    size_t *off = (size_t *)calloc((size_t)t->count, sizeof(size_t));
    int c;
    if (!off) return 0;
    if (t->att) fm_put(&o, "AT&T", 4);
    for (c = t->nodes[t->root].first; c >= 0; c = t->nodes[c].next) fm_write_node(t, c, &o, off);
    /* off[] is absolute in the output (after "AT&T"), which is what DIRM
       offsets are */
    if (!o.over && fix_dirm) fm_fix_dirm(t, &o, off);
    free(off);
    return o.over ? 0 : o.n;
}

/* ---- mutations ------------------------------------------------------------ */

static const char *const fm_ids[] = {
    "INFO", "INCL", "Sjbz", "Djbz", "BG44", "FG44", "FGbz", "TXTz", "TXTa",
    "ANTz", "ANTa", "NAVM", "DIRM", "TH44", "PM44", "BM44", "Smmr", "CIDa",
};
static const char *const fm_types[] = { "DJVU", "DJVM", "DJVI", "THUM", "PM44", "BM44" };

static int fm_pick(fm_tree *t, uint32_t *s, int want_leaf)
{
    int tries;
    for (tries = 0; tries < 16; tries++) {
        int k = 1 + (int)(fm_rand(s) % (uint32_t)(t->count - 1));
        if (k <= 0 || k >= t->count || t->nodes[k].parent < 0) continue;
        if (want_leaf == 1 && t->nodes[k].is_form) continue;
        return k;
    }
    return -1;
}

static void fm_unlink(fm_tree *t, int k)
{
    int *link = &t->nodes[t->nodes[k].parent].first;
    while (*link >= 0 && *link != k) link = &t->nodes[*link].next;
    if (*link == k) *link = t->nodes[k].next;
    t->nodes[k].parent = -1;
    t->nodes[k].next = -1;
}

/* Deep-copy src (from tree st) into t, inserted after `after` (sibling). */
static int fm_clone(fm_tree *t, const fm_tree *st, int src, int parent, int after)
{
    int k = fm_new(t), c;
    if (k < 0) return -1;
    t->nodes[k] = st->nodes[src];
    t->nodes[k].owned = NULL;
    t->nodes[k].first = -1;
    t->nodes[k].parent = parent;
    if (!st->nodes[src].is_form && st->nodes[src].n) {
        uint8_t *cp = (uint8_t *)malloc(st->nodes[src].n);
        if (!cp) return -1;
        memcpy(cp, st->nodes[src].p, st->nodes[src].n);
        t->nodes[k].p = t->nodes[k].owned = cp;
    }
    if (after >= 0) {
        t->nodes[k].next = t->nodes[after].next;
        t->nodes[after].next = k;
    } else {
        t->nodes[k].next = -1;
        fm_append_child(t, parent, k);
    }
    for (c = st->nodes[src].first; c >= 0; c = st->nodes[c].next)
        fm_clone(t, st, c, k, -1);
    return k;
}

static int fm_mutate_tree(fm_tree *t, uint32_t *s, size_t room)
{
    int k, op = (int)(fm_rand(s) % 100);
    if (t->count < 2) return -1;
    if (op < 55) { /* mutate a leaf payload */
        size_t cap, n;
        uint8_t *buf;
        k = fm_pick(t, s, 1);
        if (k < 0) return -1;
        cap = t->nodes[k].n + (room < 4096 ? room : 4096);
        if (cap == 0) return -1;
        buf = (uint8_t *)malloc(cap);
        if (!buf) return -1;
        if (t->nodes[k].n) memcpy(buf, t->nodes[k].p, t->nodes[k].n);
        n = LLVMFuzzerMutate(buf, t->nodes[k].n, cap);
        free(t->nodes[k].owned);
        t->nodes[k].p = t->nodes[k].owned = buf;
        t->nodes[k].n = n;
    } else if (op < 67) { /* delete */
        k = fm_pick(t, s, 0);
        if (k < 0) return -1;
        fm_unlink(t, k);
    } else if (op < 79) { /* duplicate */
        k = fm_pick(t, s, 0);
        if (k < 0) return -1;
        if (fm_clone(t, t, k, t->nodes[k].parent, k) < 0) return -1;
    } else if (op < 87) { /* swap with next sibling */
        int nx, prev;
        k = fm_pick(t, s, 0);
        if (k < 0 || (nx = t->nodes[k].next) < 0) return -1;
        /* move k after nx */
        prev = t->nodes[k].parent;
        fm_unlink(t, k);
        t->nodes[k].parent = prev;
        t->nodes[k].next = t->nodes[nx].next;
        t->nodes[nx].next = k;
    } else if (op < 95) { /* retag a chunk */
        k = fm_pick(t, s, 1);
        if (k < 0) return -1;
        memcpy(t->nodes[k].id, fm_ids[fm_rand(s) % (sizeof(fm_ids) / sizeof(fm_ids[0]))], 4);
    } else { /* retag a FORM type */
        int i;
        for (i = 0, k = -1; i < 16; i++) {
            int c = fm_pick(t, s, 0);
            if (c >= 0 && t->nodes[c].is_form) { k = c; break; }
        }
        if (k < 0) return -1;
        memcpy(t->nodes[k].type, fm_types[fm_rand(s) % (sizeof(fm_types) / sizeof(fm_types[0]))], 4);
    }
    return 0;
}

/* Static: the trees are ~100 KB each; libFuzzer calls these hooks serially. */
static fm_tree fm_t1, fm_t2;

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                               unsigned int seed)
{
    uint32_t s = seed ? seed : 1;
    size_t out = 0;
    uint8_t *tmp;
    /* 20%: plain byte-level mutation (keeps container-parser coverage) */
    if (fm_rand(&s) % 5 == 0 || fm_parse(&fm_t1, data, size) != 0)
        return LLVMFuzzerMutate(data, size, max_size);
    if (fm_mutate_tree(&fm_t1, &s, max_size > size ? max_size - size : 0) == 0) {
        tmp = (uint8_t *)malloc(max_size);
        if (tmp) {
            /* 90%: recompute DIRM offsets; 10%: leave them stale */
            out = fm_serialize(&fm_t1, tmp, max_size, fm_rand(&s) % 10 != 0);
            if (out) memcpy(data, tmp, out);
            free(tmp);
        }
    }
    fm_free(&fm_t1);
    return out ? out : LLVMFuzzerMutate(data, size, max_size);
}

size_t LLVMFuzzerCustomCrossOver(const uint8_t *d1, size_t n1, const uint8_t *d2,
                                 size_t n2, uint8_t *out, size_t max_out,
                                 unsigned int seed)
{
    uint32_t s = seed ? seed : 1;
    size_t r = 0;
    if (fm_parse(&fm_t1, d1, n1) == 0 && fm_parse(&fm_t2, d2, n2) == 0 &&
        fm_t1.count > 1 && fm_t2.count > 1) {
        int src = fm_pick(&fm_t2, &s, 0), dst = fm_pick(&fm_t1, &s, 0);
        if (src >= 0 && dst >= 0) {
            if (fm_rand(&s) & 1) { /* replace dst with src */
                int k = fm_clone(&fm_t1, &fm_t2, src, fm_t1.nodes[dst].parent, dst);
                if (k >= 0) fm_unlink(&fm_t1, dst);
            } else {               /* insert src after dst */
                fm_clone(&fm_t1, &fm_t2, src, fm_t1.nodes[dst].parent, dst);
            }
            r = fm_serialize(&fm_t1, out, max_out, 1);
        }
    }
    fm_free(&fm_t1);
    fm_free(&fm_t2);
    if (r) return r;
    /* fallback: head of d1 + tail of d2 */
    {
        size_t a = n1 ? fm_rand(&s) % (n1 + 1) : 0, b = n2 ? fm_rand(&s) % (n2 + 1) : 0;
        if (a > max_out) a = max_out;
        memcpy(out, d1, a);
        if (n2 - b > max_out - a) b = n2 - (max_out - a);
        memcpy(out + a, d2 + b, n2 - b);
        return a + n2 - b;
    }
}

#endif /* FUZZ_MUTATOR_H */
