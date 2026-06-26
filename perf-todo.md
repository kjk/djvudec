# Render performance — open items

Tracked regressions and ideas for closing the gap vs DjVuLibre (`ddjvuapi`
`page_render`). Byte-exact verification is done; remaining work is speed.

Bench commands (from repo root):

```text
bun cmd/bench.ts testfiles/subset/djvu3spec.djvu
bun cmd/build_dump.ts
out/msvc/djvudec_dump.exe -bench-render -layers -warm 1 testfiles/subset/djvu3spec.djvu
```

---

## Known slow case: `djvu3spec.djvu` pages 61–64

Bitonal 2550×3300 pages. DjVuLibre ~4.5–5.2 ms/page; we are ~9–13 ms/page
(+105–185%). Page 65 on the same file is ~2 ms — faster than DjVuLibre — so
this is not a generic bitonal problem.

### Snapshot (`djvu_test_msvc.exe -bench`, fastest of 3)

| Page | DjVuLibre | Ours | Δ |
|------|-----------|------|---|
| 61 | 4.46 ms | 12.70 ms | +185% |
| 62 | 4.75 ms | 12.54 ms | +164% |
| 63 | 5.17 ms | 12.30 ms | +138% |
| 64 | 4.58 ms | 9.38 ms | +105% |
| 65 | 4.21 ms | 2.00 ms | −53% |

### Layer breakdown (`djvudec_dump -bench-render -layers -warm 1`, fastest of 3)

| Page | Total | JB2 | Composite | IW44 |
|------|-------|-----|-----------|------|
| 61 | ~13.9 ms | ~11.8 ms | ~1.9 ms | 0 |
| 62 | ~13.8 ms | ~11.6 ms | ~2.0 ms | 0 |
| 63 | ~14.1 ms | ~11.9 ms | ~2.0 ms | 0 |
| 64 | ~10.8 ms | ~8.6 ms | ~2.0 ms | 0 |
| 65 | ~2.3 ms | ~0.45 ms | ~1.7 ms | 0 |

JB2 decode dominates on 61–64. Composite (`visit_ink` stamp at subsample=1) is
already reasonable (~2 ms). IW44 is not involved.

### Stream shape (page 61 Sjbz, approximate)

- Inherited shared Djbz dict is cached at `djvu_doc_open` (not re-decoded per
  render).
- Per-render cost is fresh **Sjbz** decode: ~72 `MATCHED_REFINE_LIBRARY_ONLY`
  records (cross-coded shapes against dict parents), plus many cheap
  `MATCHED_COPY` blits.
- Contrast page 65: ~17 new marks, JB2 ~0.45 ms.

Root cause: **per-render Sjbz decode** spends ~8–12 ms in
`code_bitmap_cross` / ZP arithmetic decoding (~0.1–0.15 ms per refined shape,
plus end-of-decode RLE `djvu_bm_compress` batch and per-shape `djvu_bm_bbox`
in `add_library`).

---

## Already tried (commit `6ba8a11`)

- Hot-loop rewrite of `code_bitmap_directly` / `code_bitmap_cross`: hoisted
  `djvu_zp` `a`/`fence`, direct `bitdist`/`cbitdist` indexing, row-base pointer
  arithmetic, DjVuLibre-style `up0[dx++] = n` loop.
- Removed `code_bit_arr` wrapper; batch `djvu_bm_compress` at end of
  `jb2_decode_into` instead of per-record.

Result: ~1–3% on p61 vs pre-change; gap to DjVuLibre unchanged. Incremental
bbox inside the pixel loop was tried and reverted (branch hurt more than it
saved). Skipping RLE compress on ephemeral page masks sped JB2 but doubled
composite (bytes `visit_ink` vs RLE).

---

## Future improvements (priority order)

### 1. Cache decoded page Sjbz at doc-open (largest win on repeat access)

Mirror existing caches (`jb2_dicts[]`, inline Djbz dedup, IW44 BG44/FG44):
after first `djvu_jb2_decode` per page, store `jb2_image *` on `djvu_page_int`
and reuse on `djvu_page_render`. Does not help the very first decode in a
session but matches how viewers behave (doc open once, many page paints).

### 2. JB2 first-decode: reduce work outside the ZP pixel loop

- **`add_library` / `djvu_bm_bbox`**: four-pass full-bitmap scan after every
  new shape (~72× on p61). Options:
  - Single-pass raw scan in `djvu_bm_bbox` (one read stream vs four).
  - Incremental bbox accumulated after each row in `code_bitmap_*` (not inside
    the per-pixel ZP loop — e.g. scan the row buffer once per decoded row).
  - Defer libinfo bbox until after batch RLE compress and use `bm_bbox_rle`.
- **Batch RLE compress**: profile cost of `djvu_bm_compress` for ~72 shapes;
  consider parallel compress or lazy compress (only shapes referenced by blits).

### 3. JB2 first-decode: ZP / cross-coding throughput

DjVuLibre is ~2–3× faster on the same NM/MR-heavy streams with structurally
identical decode logic — likely MSVC codegen on the tight `zp.decoder` loop.

- Build bench with **clang** on Windows (`bun cmd/build.ts -clang`) and
  compare; consider clang as default for release if consistently faster.
- **PGO** (`/LTCG /GENPROFILE` + training on `djvu3spec` / `Z:\sumtest`).
- **Open-code** `jb2_shift_*_context` + fast-path ZP in the inner `while` (no
  helper call) and verify disassembly (`objdump -d`) that `a`/`fence` stay in
  registers.
- **`djvu_bm_ensure_bytes` on cross parents**: inherited dict shapes are RLE;
  first cross-ref per parent decompresses. Track which parents are hot; keep
  decoded bytes pinned for the duration of Sjbz decode (avoid re-compress of
  inherited shapes mid-stream).

### 4. Composite (lower priority for 61–64)

`visit_ink` RLE path is ~2 ms for 8.4M-page output — not the bottleneck here.
If JB2 is fixed, revisit only if composite resurfaces on other files.

### 5. Measurement / isolation

- Add `cmd/jb2_bench.ts` (or `djvu_test -jb2bench`) to time
  `djvu_jb2_decode` alone per page (no composite), with record-type histogram
  (`DJVU_JB2_DEBUG=1`).
- Compare against `test/jb2ref.cpp` oracle on raw Sjbz bytes to separate
  decode correctness from render-path overhead.

---

## Fast paths (for context — do not regress)

- `test0.djvu` p1/2/5: bitonal subsample=1 `visit_ink` stamp (~1.5–2.5 ms,
  ~40–65% faster than DjVuLibre) — commit `e7923f9`.
- GBitmap RLE compress/blit/bbox — commit `b319c98`.
- Doc-open caches: shared INCL Djbz, inline Djbz dedup, IW44 BG44/FG44.

---

## Not perf issues

- `1998_compression.djvu` p19 render mismatch vs ddjvu (cosmetic fg-stencil
  quirk; mask/bg/fg byte-exact vs DjVuLibre internals).