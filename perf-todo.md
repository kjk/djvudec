# Render performance — open items

Tracked regressions and ideas for closing the gap vs DjVuLibre (`ddjvuapi`
`page_render`). Byte-exact verification is done; remaining work is speed.

## Bench & profile commands (from repo root)

```text
# Layer timers (djvudec only)
bun cmd/build-dump.ts
out/msvc/djvudec_dump.exe -bench-render -layers -reps 3 file.djvu

# vs DjVuLibre
bun cmd/bench.ts deps/DjVuLibre/doc/djvu3spec.djvu

# Before/after (library-only; freeze "before" binary before editing)
bun cmd/build-bench.ts before -clean
# … edit src/ …
bun cmd/build-bench.ts after -clean
# Prefer running the two exes directly so auto-rebuild does not overwrite "before"

# Profile build (MSVC: -O2 -Ob1 -Zi, no -GL/-LTCG) + WPR one-liners
bun cmd/build-prof.ts
bun cmd/build-prof.ts -print
```

Verification after any speed change:

- Byte-exact: `bun cmd/tests.ts` (corpus oracle), before/after PPM hashes, `djvu_test -verify-into`.
- Speed: layer lines from `-bench-render -layers`; true before/after binaries.

---

## Landed (do not re-do; do not regress)

| Work | Approx. effect | Notes |
|------|----------------|--------|
| Planar `map_image` + SSE2 clamp + SSE2 YCbCr→RGB (`iw44.c`) | ~6–10% on IW44 stage (color) | Byte-exact; commit on `simd` |
| SSE2 `filter_bv` interior, `scale==1` only | Small / noisy (~few % IW44) | Edges + coarser scales still scalar |
| Lazy per-page cache + public APIs | Repeat paints skip re-decode | `djvu_ctx_set_cache_per_page(1)` + lock/unlock; `djvu_doc_drop_page_cache` / `djvu_doc_page_cache_size` |
| Run-aware color stamp (`compose.c`) | **~30% composite** on FGbz palette pages (e.g. `test008C`) | `visit_ink_runs` + solid RGB fill; FG44 weaker win |
| Bitonal `visit_ink_runs` + `memset` stamp | Already fast vs DjVuLibre on light pages | Do not go back to per-pixel stamps |
| Shared Djbz / inline Djbz / IW44 layer acquire caches | Doc-wide or per-page when caching on | Shared dicts ≠ page Sjbz |

**Host must enable caching** for Sjbz/IW44/bg reuse across paints. Without
`cache_per_page` + locks + long-lived `djvu_doc`, every render re-decodes.

---

## Slowest sample pages (profiling candidates)

Sweep (`-bench-render -layers -reps 2`, min of 2) over
`djvu3spec`, `1998_compression`, `test008C`, `test064C`, `lizard2002`,
`test043C`. Wall times are machine-dependent; **order and dominant layer** matter.

| Priority | File | Page | ~total | Dominant | Why profile |
|----------|------|------|--------|----------|-------------|
| 1 | `deps/artifacts/test043C.djvu` | **p5** (also p1, p4) | ~200–215 ms | **iw44** (~all) | Purest IW44 stack |
| 2 | `deps/artifacts/test008C.djvu` | **p1**, **p5** | ~168–171 ms | iw44 + composite | Compound + stamp |
| 3 | `deps/DjvuNet/Specs/1998_compression.djvu` | **p25** etc. | ~53–60 ms | mixed | FG44 compound balance |
| 4 | `deps/DjVuLibre/doc/djvu3spec.djvu` | **p61–63** | ~7–11 ms* | **jb2** | Cold Sjbz / ZP |

\*Bitonal absolute ms may be lower than older snapshots; still the place to
profile **JB2**, not IW44. Color pages on `djvu3spec` (e.g. p21) are IW44-heavy
but smaller than `test043C`.

### Profile one page (WPR)

```powershell
bun cmd/build-prof.ts
# Admin recommended:
wpr -start CPU -filemode
out\msvc_prof\djvudec_prof.exe -bench-render -reps 8 -page 5 deps\artifacts\test043C.djvu
wpr -stop $env:TEMP\djvudec.etl
```

WPA: **Computation → CPU Usage (Sampled)** → process `djvudec_prof.exe` → stacks
under `filter_bv` / `map_image` / `code_bitmap_*` / `compose_*`.

For **instructions retired / CPI per function**: Intel VTune (or AMD uProf) on
the same `djvudec_prof.exe` + same `-page` / `-reps` args.

Always use **`-page N`** so the trace is one workload.

---

## Suggested next work (priority)

### 1. Wire page cache in the real viewer path (product)

APIs exist; speed only appears if the host:

- `djvu_ctx_set_cache_per_page(1)` + non-NULL lock/unlock  
- keeps `djvu_doc` open across paints  
- optionally LRU-evicts with `djvu_doc_drop_page_cache` / `page_cache_size`  

**Measure:** same page twice with cache on (2nd paint should drop Sjbz/IW44 to
near zero) vs cache off.

Does **not** help cold first paint.

### 2. Cold JB2 first-decode (bitonal; closes gap vs DjVuLibre)

Still the hard case on NM/MR-heavy Sjbz (`djvu3spec` p61–64 historically
~2× DjVuLibre). SIMD cannot help ZP arithmetic coding.

- **`add_library` / `djvu_bm_bbox`**: four-pass full-bitmap scan after every new
  shape. Prefer single-pass, per-row incremental bbox after decode row, or
  defer bbox until after batch RLE (`bm_bbox_rle`).
- **Dict parent bytes during one Sjbz**: avoid RLE↔bytes thrash on shared
  shapes mid-stream; pin bytes for the decode duration.
- **Codegen**: clang release on Windows vs MSVC; **PGO**
  (`/LTCG /GENPROFILE` trained on `djvu3spec` / color corpus).
- Open-code ZP + context shift in `code_bitmap_*` if disassembly shows spills.

Already tried (commit `6ba8a11` era): hot-loop rewrite of
`code_bitmap_directly` / `code_bitmap_cross`, batch RLE at end of decode —
~1–3% on p61; gap to DjVuLibre largely unchanged. Skipping RLE on ephemeral
masks sped JB2 but hurt composite (bytes `visit_ink`).

### 3. Confirm where IW44 time goes (before more SIMD)

Profile **test043C p5**:

- If **bitplane ZP decode** dominates → more lifting SIMD is low ROI.  
- If **`filter_bv` / `filter_bh` / `map_image` / YCbCr** dominate → continue
  transform path (below).

### 4. More IW44 (only if transform is hot)

- Coarse **`filter_bv` scales** (only `scale==1` interior is SSE2 today).  
- **`filter_bh`**: left-to-right recurrence — hard; DjVuLibre leaves scalar.  
- **`build_unified` / zigzag scatter** layout costs.  
- Optional **AVX2 dispatch** (runtime CPUID + scalar fallback; no
  `-march=native`; amalgamation-safe `static` names).

### 5. Composite leftovers

- **FG44** stamping: FGbz solid runs already ~30% faster composite on
  `test008C`; FG44 still samples often — tile by FG cell when mask is dense.  
- **`compose_finalize` / BGR flip**: easy bandwidth win (`pshufb`-style).  
- Bitonal stamp is already run-based — do not over-invest.

### 6. Allocation / arena

Per-render `djvu_alloc` (shapes, temps, subsample acc): **bump arena per
render**, free once; reuse scratch buffers. Helps shape-heavy pages without
changing codecs.

### 7. Parallelism

- Parallel **independent layers** on one page (BG44 ∥ FG44 ∥ Sjbz) when all
  needed — careful with shared dicts + locks.  
- Multi-page decode/render already exercised by stress tests.  
- **Not** practical: parallelize a single ZP stream.

### 8. Lower priority / situational

- Scaler: usually off hot path (cached BG).  
- Caller subsample policy (don't force full-res at low zoom).  
- Release vs profile builds: ship may keep `-GL -LTCG` / PGO; profile binary
  stays non-LTCG.  
- NEON ports of planar clamp/YCbCr/`filter_bv` for ARM.

---

## Known slow case: `djvu3spec.djvu` pages 61–64 (JB2)

Bitonal 2550×3300. Historically DjVuLibre ~4.5–5.2 ms/page vs us ~9–13 ms
(+105–185%) on cold full render. Page 65 ~2 ms (faster than DjVuLibre) — not a
generic bitonal problem.

### Older snapshot (`djvu_test_msvc.exe -bench`, fastest of 3)

| Page | DjVuLibre | Ours | Δ |
|------|-----------|------|---|
| 61 | 4.46 ms | 12.70 ms | +185% |
| 62 | 4.75 ms | 12.54 ms | +164% |
| 63 | 5.17 ms | 12.30 ms | +138% |
| 64 | 4.58 ms | 9.38 ms | +105% |
| 65 | 4.21 ms | 2.00 ms | −53% |

### Layer breakdown (older; `-layers -warm 1`)

| Page | Total | JB2 | Composite | IW44 |
|------|-------|-----|-----------|------|
| 61 | ~13.9 ms | ~11.8 ms | ~1.9 ms | 0 |
| 65 | ~2.3 ms | ~0.45 ms | ~1.7 ms | 0 |

JB2 decode dominates. Composite stamp ~2 ms. IW44 not involved.

### Stream shape (page 61 Sjbz, approximate)

- Shared Djbz cached (not re-decoded per render).  
- Per-render (cache off): fresh **Sjbz** — ~72 `MATCHED_REFINE_LIBRARY_ONLY`
  plus many `MATCHED_COPY` blits.  
- Page 65: ~17 new marks, JB2 ~0.45 ms.  

Root cost: `code_bitmap_cross` / ZP + `djvu_bm_bbox` / RLE batch — see
priority **#2**.

---

## Page-local cache (status)

**Implemented** (lazy on first acquire/render when `cache_per_page` is on):

- `pages[i].jb2_mask` (Sjbz)  
- `iw_bg` / `iw_fg`  
- `bg_native` / `bg_scaled`  

Public:

```text
void   djvu_doc_drop_page_cache(djvu_doc *doc, int page_no);
size_t djvu_doc_page_cache_size(djvu_doc *doc, int page_no);
```

Probe: `djvudec_dump -cache-probe -page 1 file.djvu`  
(`before=0`, `after_render>0`, `after_drop=0`).

Shared Djbz remains doc-wide (not in page cache size).

**Not done:** host-side LRU policy; eager full-doc Sjbz at open (avoid — open
latency). Optional: `preload_jb2_masks_range` for nearby pages only.

---

## SIMD / vectorization (color IW44) — status

Release builds: max opt, baseline **SSE2**, no forced AVX2. SIMD helpers must
stay amalgamation-safe (`static`, unique names). AVX2 only via runtime dispatch.

| Item | Status |
|------|--------|
| Planar map + SSE2 clamp + YCbCr→RGB | **Done** |
| `filter_bv` scale=1 interior SSE2 | **Done** (modest) |
| `filter_bh` | Open (serial; hard) |
| Coarse-scale `filter_bv` | Open |
| AVX2 widen of above | Open (optional) |
| `compose_finalize` BGR/flip | Open (small) |
| Bitonal run stamp | **Done** (algorithmic) |
| Color run stamp | **Done** (FGbz big win) |
| Scaler bilinear | Skip unless profiled hot |
| JB2/ZP | **Not vectorizable** |

DjvuNet SIMD inspiration was YCbCr→RGB (integer recipe matches our scalar;
byte-exact via `packus` / clamps).

---

## Fast paths (do not regress)

- Bitonal subsample=1 run stamp (often faster than DjVuLibre on light pages).  
- GBitmap RLE compress/blit/bbox.  
- Doc caches: shared INCL Djbz, inline Djbz dedup, IW44 acquire, optional
  per-page layers.  
- FGbz top-down run fill + general composite run stamp.  
- O(ink) subsample coverage (bitonal + color stencil).  

---

## Not perf issues

- `1998_compression.djvu` p19 render mismatch vs ddjvu (cosmetic fg-stencil
  quirk; mask/bg/fg byte-exact vs DjVuLibre internals).

---

## Methodology cheat-sheet

| If `-layers` / WPA says… | Next lever |
|--------------------------|------------|
| Time only on **2nd** paint | Host page cache wiring |
| `code_bitmap_*` / ZP | JB2 cold path (#2) |
| `filter_bv` / `filter_bh` / `map_image` | IW44 (#3–4) |
| `compose_stamp*` / stencil | FG44 tiling / finalize (#5) |
| `djvu_alloc` / free | Render arena (#6) |

Change one thing → true before/after binaries → corpus byte-exact → re-bench
dominant layer, not only total ms (system noise often moves jb2 ±5% too).
