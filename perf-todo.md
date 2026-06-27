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

---

# SIMD / vectorization opportunities (color IW44 path)

Separate track from the JB2 work above. Profile-backed; inspiration cross-read
from DjvuNet's SIMD (`deps/DjvuNet/DjvuNet/Wavelet/InterWaveTransform.Vector128/256.cs`).

## Build reality
Both toolchains compile at max opt (`clang -O3`, `cl -O2 -Ob3 -GL`) but **neither
passes `-march` / `-arch:AVX2`** — codegen is baseline **SSE2 (128-bit), no
AVX/AVX2**. Clang auto-vectorizes simple contiguous loops to SSE2 but bails on
strided access, per-pixel callbacks, and edge-case-laden filters. Headroom in
both *width* (enabling AVX2) and *loops the compiler can't touch*.

Any SIMD must fold into the single-TU amalgamation (`dist/djvu.c`) — keep helpers
file-local (no two `.c` sharing a `static` name). For AVX2, use a runtime CPUID
dispatch with a scalar fallback (lib ships to unknown targets); do **not** use
`-march=native`.

## Where time goes (color/compound pages)
`djvudec_dump -bench-render -warm 1 -layers` (fastest of 3, warm caches):

| Page kind (example)                           | total   | dominant layers                         |
|-----------------------------------------------|---------|-----------------------------------------|
| Compound color, full-res (`1998_compression`) | ~15 ms  | **iw44 ~4.3 ms**, **composite ~2.3 ms** |
| Color screen, small (`mtorrent`)              | ~0.3 ms | iw44 0.09 ms                            |

(Bitonal text pages are JB2/stamp-bound — see the JB2 section above and #4 below.)
ZP / arithmetic decoders are inherently serial bit-at-a-time — **not vectorizable**.

## What DjvuNet vectorizes (the inspiration)
DjvuNet does **not** SIMD the wavelet lifting. It vectorizes the **YCbCr↔RGB color
transform**. `TransformYCbCrToRgbVector256` is op-for-op identical to our scalar
loop in `iw44.c` `iw44_render_rgb_impl` (~lines 652-665):

```
DjvuNet AVX2          our scalar (iw44.c)
blue>>2               t1 = bv>>2
red + red>>1          t2 = rv + (rv>>1)
luma+128              yv + 128
luma128 - temp1       t3 = yv+128 - t1
luma128 + temp2       tr  (red out)
temp3 - temp2>>1      tg  (green out)
temp3 + blue<<1       tb  (blue out)
Max(0,Min(255,..))    clamp255
PackUnsignedSaturate  (uint8_t) store
```

Same integer math → SIMD port verifies **byte-exact** vs scalar (`-verify-into` /
corpus oracle).

## Ranked opportunities

### 1. Planar `map_image` + SIMD clamp + SIMD YCbCr→RGB — best risk/reward, DjvuNet-proven  [DONE]
Implemented in `iw44.c` (SSE2, baseline on x86-64, scalar fallback elsewhere —
no CPUID dispatch needed for SSE2). `map_image` now writes contiguous **planar**
Y/Cb/Cr; `clamp_row_s8` does `(c+32)>>6` via `packs_epi16` (signed saturation =
[-128,127] clamp); `ycbcr_to_rgb_row` / `gray_to_rgb_row` do the conversion 8 px
at a time (`packus_epi16` = [0,255] clamp), with the bottom-up flip as a
row-reorder. Dropped the wasteful full-buffer memset. Byte-exact (corpus oracle
245 match / 0 mismatch, ASan clean). IW44 color-render layer **−3 to −4 %**
(min-of-8: lossy_masked 46.5→45.1 ms, compression 111.9→107.7 ms over all pages);
A/B confirmed the SSE2 conversion beats scalar-planar by ~5 %. AVX2 (CPUID
dispatch) would widen this further — deferred.

Original plan:
`map_image` (`iw44.c:277`) writes int8 with stride (`pixsep=3`, interleaved
Y/B/R); `iw44_render_rgb_impl` reads it back interleaved and writes to a *flipped*
index. Two SIMD blockers: clamp loop scatters (stride 3), color loop reads
interleaved + writes flipped.

Fix: `map_image` writes **planar** (pixsep=1, contiguous) into 3 buffers. Then:
- clamp `(x+32)>>6` to [-128,127] = load int16 → add → shift → `packs_epi16`
  (signed saturation *is* the [-128,127] clamp) → packed store;
- YCbCr→RGB loads 3 contiguous planes (no deinterlace shuffle DjvuNet needs),
  runs the recipe above, interleaves to RGB. Flip done as a row-reorder, not a
  per-pixel flipped index.

Impact: removes strided scatter, unlocks color+clamp portion of the 4.3 ms iw44
layer; also speeds the scalar path (contiguous).

### 2. `filter_bv` vertical lifting, interior rows — largest compute, more work
`filter_bv` (`iw44.c:114`) is **column-parallel**: every x does identical math
with neighbors at rows ±s/±3s. At `scale=1` (finest, every coefficient) neighbors
are contiguous int16 rows → ideal (8×int32 lanes to stay overflow-safe on
`(a<<3)+a`). Interior case (`y∈[3,h-3]`) is the bulk; edges stay scalar.
`filter_bh` (`iw44.c:184`) is a left-to-right recurrence → not x-vectorizable
(row-parallel only, transpose-heavy). DjVuLibre/DjvuNet leave both scalar.
Biggest potential single win but most code + most numerically delicate. Do after #1.

### 3. `compose_finalize` / `djvu_flip_rgb_bottomup` — small, easy
`compose.c:186` / `scaler.c:243`: row flip + optional R↔B swap = `pshufb` + copy.
Gamma branch is a 256-LUT gather (leave scalar). Low effort, small payoff.

### 4. Bitonal ink-stamp — algorithmic, not pure SIMD, ~2 ms on text pages
`render.c` stamps each ink pixel via indirect call through `djvu_bm_visit_ink`
(`bitmap.c:392`) — per-pixel function pointers can't vectorize. The RLE path knows
ink **runs**; replace the per-pixel callback with a run-aware sink (`memset(0)` of
`[left+col, left+nc)` on the dest row). Structural refactor; highest value for
text corpora. (Overlaps the composite item in the JB2 section.)

### 5. `scaler.c` bilinear — skip
Runs at doc-open (BG44 upscale cached), off render hot path; `s_interp` lookups
are gather-bound.

## Verification
- Byte-exact: `bun cmd/tests.ts` (corpus oracle), `djvu_test -verify-into`.
- Speed: `cmd/build_bench.ts before/after` + `cmd/bench_perf.ts` (`-warm 1 -layers`).