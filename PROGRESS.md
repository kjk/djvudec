# djvu — C port of DjVu decoding (DjvuNet → C, jbig2dec style)

Goal: a plain-C library that decodes DjVu files enough to:
- enumerate pages + report page info (dimensions, dpi, rotation)
- produce a decompressed bitmap for a page (bitonal, gray, color)
- extract page text

We are given the whole file up-front (no incremental fetch). Decode only — no encoders.

## References (checked out locally)
- C# source being ported:   `deps/DjvuNet/DjvuNet`            (DjvuNet repo)
- Verification oracle:      `deps/DjVuLibre`                  (DjVuLibre repo)
- Test corpus: the `.djvu` files in the `deps/` checkouts (DjVuLibre/doc,
  DjvuNet/Specs, DjvuNet/DjvuNetTest/TestFiles, artifacts{,/data,/wikimedia};
  ~115 files, enumerated by `cmd/corpus.ts`; `DJVU_SPECS=<dir>` overrides).
  There is no `testfiles/` mirror any more (see AGENTS.md).
- Spec: https://www.sndjvu.org/spec.html  (code is the more definitive reference)

## Reference tools (built from DjVuLibre, see cmd/build.ts `buildRef()`)
Built into `ref_build/`:
- `ddjvu.exe`    — `ddjvu -format=pgm -page=N in.djvu out.pgm`  (bitmap oracle)
- `djvutxt.exe`  — `djvutxt --page=N in.djvu`                   (text oracle)
- `djvused.exe`, `bzz.exe` — outline/annotation oracles, BZZ round-trip
(djvudump crashes in this build; not used.)

## C API (src/djvu.h) — jbig2dec flavor
Opaque `djvu_ctx` / `djvu_doc`; caller-supplied alloc/free/error (and optional
lock/unlock) callbacks. See header.

Internal headers are consolidated into a single `src/djvu_internal.h` (one
labeled section per module: core, zpcodec, bitmap, bzz, jb2, iw44, scaler,
compose, debug). Every `.c` file includes just that one header.

## Architecture / port map
| C module            | from C# (DjvuNet)                       | status |
|---------------------|-----------------------------------------|--------|
| (readers inline)    | IO/DjvuReader.cs (BE/LE in djvu_internal)| DONE   |
| zptable.c           | Compression/ZPCodec.cs default table    | DONE   |
| zpcodec.c           | Compression/ZPCodec.cs (decode only)    | DONE   |
| document.c          | Parser + DataChunks (DJVM/DIRM/INFO)    | DONE   |
| bzz.c               | Compression/BSInputStream (decode)      | DONE   |
| jb2.c               | JB2 modules (decode)                    | DONE   |
| bitmap.c            | Graphics/Bitmap (GBitmap)               | DONE   |
| iw44.c              | Wavelet/* (decode)                      | DONE   |
| text.c              | Text/PageText + TXTa/TXTz chunks        | DONE   |
| compose.c           | DjvuImage composite (mask+fg+bg)        | DONE   |
| scaler.c            | Graphics/PixelMapScaler (GPixmapScaler) | DONE   |
| render.c            | DjvuPage (layer selection, rotation)    | DONE   |
| outline.c           | NavmChunk + Navigation/Bookmark         | DONE   |
| annot.c             | AntaChunk/AntzChunk maparea links       | DONE   |
| iw44_zigzag.c       | zigzag table (InterWaveCodec)           | DONE   |
| bufread.c, debug.c  | (no C# counterpart: readers, harness hooks) | DONE |

## Status: feature-complete; verified byte-for-byte vs DjVuLibre
`bun cmd/tests.ts -all` over the deps/ corpus: the most recent full MSVC runs
recorded in the change log below had 2489 matching renders; the only failures
are the two known DjVuLibre oracle quirks (see AGENTS.md).
(Historical, from the old 11-file `testfiles/` corpus: render MATCH=398
MISMATCH=1; text MATCH=353.)
`DJVU_SPECS=<dir> bun cmd/tests.ts -all` (point at any directory of .djvu) on
real-world sets (historical numbers; tests.ts replaced the old Python verify_dir.py):
  - Z:\sumtest (36 files, up to 1177 pages): render 138/3 (the 3 = fg-stencil
    ddjvu quirk, GPU Gems ~2-9px), text 141/141.
  - Z:\backup\books (1396 files; two 30-file random samples): render 300/300,
    text 300/300 -- byte-exact.
Handled from real-world testing: rotation; pure-photo (BG44-only) pages; blank
pages (INFO only -> white); large shared dicts with mid-stream numcoder resets;
gamma correction (target 2.2 / document gamma, matching ddjvu) for documents
whose INFO gamma != 2.2.
- The 1 render mismatch is 1998_compression p19 (680 px / 0.008%). Diagnosed:
  NOT a decode bug. Verified byte-for-byte against DjVuLibre internals:
    * our JB2 mask == JB2Image::get_bitmap (0 diff, whole image)
    * our background == ddjvu -mode=background (0 diff)
    * our FG44 == IW44Image (0 diff); all 845 blit positions/sizes match.
  It is a ddjvu three-layer-stencil quirk: for a few shapes in one text line,
  ddjvu paints the FG color ~1px offset from the actual JB2 mask (it inks a
  mask-background pixel and skips a mask-ink one). Our compositor aligns FG
  exactly with the mask. (debug hooks: `djvu_ctx_set_no_compose`, DJVU_JB2_BLITS,
  DJVU_JB2_SHAPE=N; reference tools test/jb2ref.cpp, test/iw44ref.cpp.)
- Color (compound/photo) pages composite at any subsample (see the
  "subsampled color composite" change-log entry); subsample==1 is byte-exact,
  subsample>1 is checked statistically by `cmd/verify-subsample.ts`.

## Milestones
1. **Page info** (no codecs): DJVM/DIRM + INFO → page count + dims. ✅ DONE
   (all 11 Specs files match ddjvu dims)
2. **BZZ** decompressor. ✅ DONE (round-trips vs `bzz -e`; decodes real DIRM)
   - full DIRM parse: component ids/types resolved (INCL resolution ready)
3. **ZP + JB2** → bitonal page bitmap. ✅ DONE
   All 122 pure-mask pages match `ddjvu -format=pgm` byte-for-byte.
   (`bun cmd/tests.ts`)
5. **Text extraction** (TXTz, BZZ); verify vs `djvutxt`. ✅ DONE
   All 144 text pages match djvutxt content (modulo trailing page separator).
4. **IW44** decoder. ✅ DONE
   BG44 + FG44 decode byte-for-byte vs DjVuLibre IW44Image (26/26 BG/FG images,
   color, via test/iw44ref). NOTE: IW44/GBitmap are stored bottom-up; output is
   flipped to top-down (save_ppm in DjVuLibre writes bottom-up).
7. **Composite** (compose.c): background (GPixmapScaler-upsampled) + foreground
   (FGbz palette two-layer, or FG44 three-layer nearest-upsample) stenciled
   through the JB2 mask. ✅ DONE — full color pages match `ddjvu -format=ppm`
   byte-for-byte (background alone matches `ddjvu -mode=background`).
6. **Subsampled rendering** at any integer subsample: bitonal via O(ink)
   coverage, color via a reduced-size composite. ✅ DONE

## Build / test
`bun cmd/get-deps.ts` — clones DjvuNet, DjVuLibre and DjvuNet/artifacts into deps/.
`bun cmd/build.ts` — builds ref tools (once), the C library + test harness
(MSVC by default on Windows, `-clang` for clang; clang elsewhere).
`bun cmd/tests.ts <-all | -rand N | file.djvu ...>` — the test driver: ensures
deps, builds, then verifies the selected corpus files. See AGENTS.md for flags.

## Known unported chunks / decode gaps
Decode-path features present in DjvuNet (C#) that this port does NOT handle yet.
Verified against `src/*.c` chunk dispatch. We DO handle: INFO, DIRM, NAVM, INCL,
Sjbz, Djbz, BG44, FG44, FGbz, PM44, BM44, TXTz, TXTa, ANTa, ANTz (including
standalone FORM:PM44 / FORM:BM44 files).

- **`BGjp` / `FGjp` — JPEG-coded bg/fg layers.** No JPEG decoder in the C port;
  a page whose background/foreground is JPEG (instead of IW44) renders blank.
  Rare. The only real correctness gap besides Smmr; would need a JPEG decoder.
- **`Smmr` — CCITT-G4/MMR-coded bitonal mask** (legacy alternative to JB2 Sjbz).
  Not handled; such pages get no mask. Rare.
- **`TH44` / `FORM:THUM` — embedded thumbnails.** Not decoded or exposed (no
  thumbnail API). Pure convenience: full-page render already works. C# exposes
  `DjvuPage.Thumbnail`.
- **`Wmrm` (watermark-removal), `CIDA` (obsolete).** Not handled; negligible,
  normally ignored even by DjVuLibre.
- **Annotation richness (partial).** We extract hyperlink mapareas
  (rect/oval/text + url + comment; poly/line mapareas are skipped because they
  have no axis-aligned box, matching SumatraPDF); we do NOT model highlight color,
  border styles, pushpins, etc. that C#'s `Annotation` carries.

Deliberately OUT of scope (not gaps): all encoders/writers (no DjVu writing),
external image-format I/O (ImageConverter, PBM/PGM/RLE serialization), and
progressive/incremental decoding (we decode the whole buffer up-front). SIMD
wavelet paths, caching, and multithreading are impl details, not features.
NB: we render INFO rotation (render.c, `image_rotate_cw`); the C# port does not.

## Change log (most recent first)
- colour composite only when DjVuLibre would (diff-fuzz finding):
  `DjVuImage::get_pixmap` + `stencil` require a background with reduction
  1..12 and, with a mask, a page-sized mask plus a foreground (FGbz palette
  with one colour index per blit, or FG44 with reduction 1..12); otherwise
  ddjvu renders the bitonal mask. We composited anyway (black FG when no FG,
  FG44 reduction clamped to 1, palette/blit count mismatch, BG reduction
  13..15). `djvu_compose_page_legal` (IW44 dims from chunk headers, no
  decode) now gates both render paths; an illegal compound page renders its
  mask expanded to RGB24, keeping the format `djvu_page_render_info`
  promises. Regression seeds: fuzz/crashes/diff (11).
- JB2 blit positions wrap like DjVuLibre's (diff-fuzz finding): JB2Blit
  stores left/bottom as unsigned short, so a decoded bottom of -12 is 65524
  (off the page, not drawn) where we clipped the blit and painted its visible
  rows. `img_add_blit` truncates to uint16; decoder state stays int on both
  sides, so this is exact. Regression seeds: fuzz/crashes/diff (3).
- fuzzing overhaul (`bun cmd/fuzz.ts` with no args now does the best default):
  per-codec libFuzzer targets (`test/fuzz_<name>.c`: doc, jb2, iw44, bzz,
  text, annot, outline, diff) seeded by `test/fuzz_seeds.c` from the corpus,
  round-robin `-fork` over all cores; cheap per-input cost (doc corpus =
  standalone pages + small files, adaptive subsample, no trace-cmp in the
  numeric kernels: doc target from < 1 to ~15 exec/s, codec targets
  20-3000/s); per-target dictionaries (`fuzz/dict/`); an IFF-aware custom
  mutator for whole documents (`test/fuzz_mutator.h`, ~10% more edges in an
  A/B); a differential target vs DjVuLibre (`test/fuzz_diff.c`, see AGENTS.md)
  whose first runs found the five divergences below.
- fuzz slow-unit triage (slow-unit-321bf8…, slow-unit-2e9c53…): byte-identical
  copies of the seeds `test041C.djvu` (14 × 9094x5630) and `test076C.djvu`
  (18 × 4960x7015), not mutations and not decoder bugs — ~700 Mpixels of
  full-res composite per input, 10-12 s under ASan on a 2-core Linux box, over
  the 20 s threshold under `-jobs` load. Fix in the harness: `fuzz_target.c`
  caps full-res rendered pixels per input (`FUZZ_MAX_FULLRES_PIXELS`, 128 Mpx);
  later pages still get the subsample-4 render, text, zones and links. Now
  2.9 s / 6.3 s. Artifacts deleted (duplicates of corpus files).
- cross-thread abort test (`djvudec_thread -abort`, `bun cmd/thread.ts -abort`):
  workers render through the `_abortable` variants while another thread fires
  token aborts, ctx-wide aborts and page-cache drops; renders must be "aborted"
  or identical to a reference. It found two bugs on its first runs:
  * an abort during the FG44 decode (IW44 checkpoints, 05c9e48 -- already in
    the pushed dist/) made `compose_to_bg` treat the page as having no
    foreground: at subsample > 1 (no stencil checkpoint) the render returned
    a WRONG image instead of failing. Fixed by an explicit check after the FG
    acquire, a checkpoint in `compose_stencil_sub`, and a safety net at every
    public render exit (a render that saw an abort reports aborted);
  * once the scaler got an abort checkpoint, a background scale stopped
    midway left a half-written `bg_scaled` in the page cache (non-NULL =
    "complete"), so later renders of that page were wrong for good. Before
    this change that path was reachable only on out-of-memory. Fixed:
    `djvu_cpix_scale_ratio` frees its output on failure and
    `compose_bg_native_build` drops both cached backgrounds.
  Also new: abort checkpoint every 64 rows in the scaler (renders from cached
  layers were not abortable). Under TSan (WSL, clang 21): no reports. CI runs
  it on Windows, Linux, and under TSan on Linux.
- dist/djvu.c no longer carries test-harness declarations or helpers:
  `DJVU_DEBUG_ONLY_BEGIN/END` regions (the `djvu_debug_*` declarations, the
  debug-only `djvu_iw44_render_rgb` / `djvu_iw44_render_plane`) are stripped by
  build-dist; the bench harness builds from a debug amalgamation in
  out/amalg_debug that keeps them and includes src/debug.c.
- the 62 "cosmetic" corpus pages, investigated: not decoder bugs, and NOT the
  1998_compression fg-stencil quirk the docs had blamed. All are palette
  (FGbz) pages; on each, our JB2 mask equals DjVuLibre's (ddjvu -mode=mask, 0
  px) and no paper pixel differs. DjVuLibre's two-layer compositor zeroes the
  mask and *adds* each colour group's colour (clipped at 255), so where blits
  of 2-3 colour groups overlap ddjvu paints the clipped sum; we paint one
  blit's colour. The blanket verifier tolerance (<= 2000 px, <= 0.01%) is
  replaced by that exact rule (`image_compare`: ours = palette A, ddjvu =
  clip(A + C1 [+ C2])), reported as `ok palette-overlap`. Corpus: MATCH=2488
  (palette-overlap=62) MISMATCH=2 (1998_compression p19, which is FG44 and
  now reports as a mismatch again, and nasa001C). Mutation test: bumping one
  blit's red channel by one level now fails 10 of 12 test061C pages (the old
  tolerance passed it).
  Coverage gap found and closed: -verify-render compares in BGR, which
  always uses the pixmap composite, so the RGB subsample-1 direct stencil
  (`compose_page_direct_rgb`, what most API users get) was never checked
  against anything independent -- -verify-into compared it only with
  render_into, which uses the same path. -verify-into now also renders every
  colour page on a BGR context and requires equality with R/B swapped
  (same mutation on the direct path: 117 page renders flagged).
- ThreadSanitizer build: `-tsan` on `stress-test.ts`, `thread.ts`,
  `test-cache-race.ts` (`out/clang_tsan/`, Linux/macOS only); CI runs all
  three on Linux. Local run in WSL (clang 21) on djvu3spec, djvu2spec,
  test001C, test043C: no reports. The `djvu_abort.requested` flag is now
  read/written with atomic builtins (a plain volatile int is a C11 data race
  when the token is requested from another thread).
- JB2 abort checkpoint inside shape bitmaps: `code_bitmap_directly` and
  `code_bitmap_cross` poll every 64 rows, so a single huge shape (up to
  65535x65535) no longer delays an abort until its whole bitmap is decoded.
- `src/debug.c` is no longer in the amalgamation or the WASM drop
  (`DIST_SRCS` in cmd/util.ts); the bench harness links it as a separate
  object. The per-file builds (`build.ts`, `build-lib.ts`) still include it.
- CI fixes (runs 45212217901 / 45238626650). Windows: the runner's clang is
  older than its MSVC STL accepts (STL1000 "expected Clang 22 or newer")
  when building the DjVuLibre reference; the Windows DjVuLibre flags now
  define `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`. Linux fuzz-crash job:
  LeakSanitizer caught a real leak on the new `crash-deep-navm-outline` seed
  -- when a nested bookmark failed to parse, `parse_item` set
  `nchildren = i`, excluding the failed child, which already owned a
  title/url and partial children. Now `i + 1` (slots are zeroed); same fix
  in `parse_zone` (text.c). The fuzz target now aborts if any allocation is
  still live after close, so leaks fail on Windows too (Windows ASan has no
  leak checker). UBSan: the IW44 lifting steps wrote 9*a as `(a << 3) + a`
  with a negative `a`; now a multiply (same output).
- JB2 abort checkpoint: `jb2_decode_into` polls `djvu_aborted` once per
  record, covering the page mask and shared dictionaries decoded lazily
  inside a render (lock hooks). An aborted decode returns NULL, is not
  cached, and logs neither "JB2: decode failed" nor the preload warnings.
  Dict pre-decode at `djvu_doc_open` is unaffected (`djvu_aborted` is only
  true inside a render).
- IW44 abort checkpoints: `djvu_iw44_decode_chunk` polls `djvu_aborted` once
  per slice and `iw44_render_rgb_impl` around each plane's inverse transform,
  so photo pages (and the IW44 phase of compound pages) can be aborted
  mid-decode; before, the only checkpoint was at composite entry. An aborted
  decode is freed, never cached, and logs no warning. `djvu_aborted` now also
  requires the thread to be inside a render on that ctx (new
  `djvu_render_ctx_tls`), since IW44 decode is reachable from non-render
  paths where the thread's epoch snapshot is stale. `-verify-abort` sweeps
  photo pages in flight now: test043C 32 -> 545 aborted renders, 0 failures.
- audit follow-up (tooling + harnesses; decoder change is the JB2 one only):
  - decoder: `code_record` now refines against a private copy of ANY
    inherited (dict) reference shape, not only RLE-compressed ones -- raw
    zero-size dict shapes are never compressed and `djvu_bm_set_min_border`
    could reallocate them under concurrent readers
    (`djvu_bm_uncompress_copy` -> `djvu_bm_private_copy`). Removed the uncalled
    `djvu_debug_render_iw_gray` / `djvu_iw44_render_gray`.
    `djvu_ctx_set_lazy_iw44` stays: SumatraPDF's `libsumatrapdf.def` exports it.
  - abort API is now tested: `djvu_test -verify-abort` / `cmd/test-abort.ts`
    (aborts injected from the allocator callback at every allocation count;
    no partial image, no leak, next render identical; cache off and on). No
    decoder bug found. Noted: IW44 decode has no abort checkpoint, so a photo
    page (or the IW44 phase of a compound page) runs to completion once started.
  - (superseded by the palette-overlap rule above) the verifier's cosmetic
    tolerance (RGB page within 2000 px / 0.01% of ddjvu) is now reported
    (`ok cosmetic`, `MATCH=N (cosmetic=K)`) instead of
    silent; it covers the fg-stencil quirk pages incl. 1998_compression p19.
  - `tests.ts`: unopenable/unreadable files fail instead of passing as
    "0 pages"; selection via `selectFiles`.
  - bench: `bench.ts` builds its own harness (`out/*_bench/djvu_bench_*`) from
    an amalgamation in `out/amalg` -- it no longer rewrites `dist/`, builds
    WASM, or shares an exe with `build()`; exes relink when any object is
    newer. `-bench`/`-bench-sum` timed regions are symmetric (buffers
    allocated/touched/freed outside the clock on both sides), so numbers are
    not comparable with earlier entries. `bench-perf.ts` no longer silently
    rebuilds the "before" snapshot from edited sources.
  - harnesses: shared helpers in `test/harness_util.h`; thread joins work
    above 64 threads; `djvudec_stress` reports open/read failures as failures;
    `djvu_test` has full usage and rejects unknown flags; `-dump-features`
    and ~200 lines of uncalled `bench_ddjvu.cpp` code removed;
    `oversize_iw44` can no longer pass vacuously.
  - scripts: shared `cmd/util.ts` (one source list, one `needsRebuild`);
    new `test-cache-race.ts` (builds `test/crash_cache_race.c`), `test-abort.ts`;
    `build-dist.ts -no-wasm`; `verify-subsample.ts` builds its own tools;
    removed `verify-one.ts`, `debug-chunk.ts`, `perf-mac.ts`,
    `ensure-ascii-names.ts`, `find-slower-pages.ts` (its parser had been
    matching nothing since the bench output was compacted, so the
    `slower-*.txt` files it wrote said "0 slower pages" regardless) and
    `slower-mac-clang.txt`. CI runs the new tests and builds WASM once.
- whole-project audit pass. Decoder fixes: stack overflow on crafted input in
  the NAVM outline parser (chain of single-child bookmarks; a 78-byte file)
  and the ANTa/ANTz s-expression parser (run of `(`; a 54-byte file) -- both
  now depth-limited to 64, with regression seeds in `fuzz/crashes/`
  (`crash-deep-navm-outline`, `crash-deep-antz-links`); bitonal subsample >= 16
  used an 8-bit per-cell coverage counter that saturated (a 25%-covered cell
  rendered black at subsample 32) -- now 16-bit (vs ddjvu at subsample 24 on
  djvu2spec p2: mean abs diff 22.9 -> 8.1; the remainder is ddjvu's own
  scaled path above subsample 12); FG44 layer leaked from `compose_to_bg`
  when its pixmap alloc/render failed; `djvu_page_render_info(NULL, ...)`
  dereferenced NULL; JB2 library-array allocation failure looped forever in
  `shape2lib_set` / could leave `libinfo` short; scaler coord-table
  allocation was unchecked; two left-shifts of negative values (zpcodec init
  on empty input, BZZ BWT position packing). Removed unreferenced internals
  (`djvu_doc_preload_*_range`, `djvu_doc_iw44{,_by_form}`,
  `djvu_doc_drop_page_iw44`, `djvu_doc_jb2_{mask,dict,dict_inline,dict_for_form}`,
  `djvu_bm_blit`, `djvu_bm_visit_ink`, `djvu_cpix_scale`), deduplicated the
  FGbz palette parse, dropped redundant zero-fills (IW44 planes, cpix,
  RLE white runs) and took `djvu_page_get_type` from the open-time chunk
  index. Fuzz target now also drives `djvu_page_text_get_zones` and
  `djvu_page_get_links`. `tests.ts`: a leak-only exit (4) is reported as a
  leak instead of a retried "crash", and leaks/crashes now fail the run's
  exit status. Corpus unchanged: render MATCH=2489 (+ the nasa001C oracle
  quirk), text MATCH=1933; ASan `-rand 12` and `fuzz.ts -check-crashes` clean.
- red=3 background-expansion speed pass on `deps/artifacts/test041C.djvu`
  (14 palette-compound pages, mostly 9094x5630): whole-document winperf
  profiling put `scaler_expand_row3` at 17.1% self / 31.9% inclusive, ahead
  of IW44 bucket decode at 10.5%. Added a 2 KB packed delta table containing
  the exact 6/16 and 11/16 interpolation results for every possible byte
  difference. Each RGB source pair now uses three L1-sized lookups instead of
  six signed multiply/round sequences. The post-change profile reduced
  `scaler_expand_row3` to 13.9% self / 30.1% inclusive. Three alternating
  MSVC release runs of open + every page + close improved from 1987.7 ms to
  1930.0 ms average (-2.9%). A cold-session comparison is 3809.71 ms
  DjVuLibre vs 1896.59 ms djvudec (-50.2%). Target verification is 14/14
  byte-exact under MSVC, clang, and ASan; the full MSVC corpus had 2489
  matching renders and only the existing unrelated DjVuLibre oracle error on
  399-byte `nasa001C.orig.png.djvu`. The regenerated amalgamation compiled
  cleanly under clang, MSVC, and Emscripten.
- direct FG44 composite speed pass on `deps/artifacts/test032C.djvu` (9
  5196x7322 compound pages): whole-document winperf profiling put red=3
  background expansion at 19.1% self, IW44 bucket decode at 15.7%, and the
  final bottom-up-to-top-down frame copy at 11.1%. Extended the existing
  full-resolution palette fast path to FG44: render the background directly
  into the top-down RGB destination, then stencil FG44 ink runs into that same
  buffer. BGR, gamma-corrected, subsampled, and page-cached renders retain the
  old general path. This removes one full 114 MB intermediate frame/copy per
  page. Three alternating MSVC release runs of open + every page + close
  improved from 1133.6 ms to 1065.5 ms average (-6.0%); the final profile has
  no `memcpy` hotspot. A cold-session comparison is 1982.26 ms DjVuLibre vs
  1015.84 ms djvudec (-48.8%), with 158.76 MB peak allocation. Target
  verification is 9/9 byte-exact under MSVC, clang, and ASan, and
  `-verify-into` is 27/27; the full MSVC corpus had 2489 matching renders and
  only the existing unrelated DjVuLibre oracle error on 399-byte
  `nasa001C.orig.png.djvu`. Also removed the now-unused generic
  `djvu_zp_decode_iw` helper exposed by strict amalgamation compilation after
  the preceding IW44-local ZP optimization.
- IW44 entropy-decode speed pass on `deps/artifacts/test043C.djvu` (5
  high-resolution photo pages): whole-document winperf profiling put
  `decode_buckets` at 54.7% self / 67.7% inclusive, plus 8.1% self in its
  out-of-line ZP renormalization path. The IW44 hot loop now keeps the ZP
  interval and fence local through both fast and renormalization paths,
  specializes coefficient-state reconstruction for the reset nonzero bands,
  and removes branches from parent-context counting and coefficient sign
  restoration. Three alternating MSVC release runs of open + every page +
  close improved from 542.4 ms to 507.7 ms average (-6.4%); isolated page
  renders improved 7.6-8.7%. The final profile has no separate renormalization
  hotspot (`decode_buckets` is 66.1% inclusive), and a cold-session comparison
  is 680.06 ms DjVuLibre vs 503.31 ms djvudec (-26.0%). Target verification is
  5/5 byte-exact under MSVC, clang, and ASan; the full MSVC corpus had 2489
  matching renders and only the existing unrelated DjVuLibre oracle error on
  399-byte `nasa001C.orig.png.djvu`.
- whole-document render profiling and integer background-scale speed pass:
  `bun cmd/prof.ts file.djvu -document -runs N` now marks complete decoder
  sessions (`djvu_doc_open`, render every page, `djvu_doc_close`) for winperf,
  and the dump harness exposes the underlying `-profile-document N` mode.
  Profiling all 48 pages of `djvulibre-book-ru.djvu` showed the generic
  horizontal GPixmapScaler loop at 48.5% self / 66.2% inclusive samples: most
  compound pages expand a 276x390 BG44 layer to 3307x4678 at red=12, with two
  pages using red=4. Added grouped integer expansion plus byte-exact unrolled
  prepare_coord phase cycles for red=4 (`2,6,10,14`) and red=12
  (`0,1,2,4,5,6,8,9,10,12,13,14`), including partial final source pairs and
  clamped tails. Alternating MSVC release runs of open + all pages + close
  improved from 1014.9 ms to 791.0 ms average (-22.1%); the IW44 stage on a
  typical red=12 page fell from ~28-29 ms to ~18-19 ms. Final cold-session
  comparison was 1881.18 ms DjVuLibre vs 813.31 ms djvudec (-56.8%).
  Target verification is 48/48 byte-exact under MSVC, clang, and ASan; the
  full MSVC corpus had 2489 matching renders. Its sole reported failure,
  399-byte standalone `nasa001C.orig.png.djvu`, is an unrelated DjVuLibre
  oracle error and produces identical SHA-256 output in the before/after
  djvudec binaries.
- merged cmd/bench-sum.ts into cmd/bench.ts as the `-like-sumatra` flag
  (runs `djvu_test -bench-sum`, the SumatraPDF engine render path, instead
  of the bare full-res `djvu_test -bench`).
- corpus tooling: `cmd/djvu-parse.ts` — pure-TS structural DjVu parser (IFF
  walk + chunk headers only; no ZP/BZZ/JB2/IW44 decode): parseDjvu() returns
  page count, per-page resolution/dpi/rotation/kind, chunk inventory, IW44
  header info (dims, color vs gray, progressive chunks), FGbz palette,
  text/anno/outline presence, shared/inline dicts, standalone PM44/BM44
  forms, and a `features` fingerprint for picking a feature-covering test
  subset. `cmd/djvu-info.ts` prints it (same selection flags as the other
  scripts; `-features` for one-line-per-file). All 115 corpus files parse
  clean. bench.ts (and bench-sum.ts) print each file's relative path + size
  (human-readable and exact bytes) before benching; `bench.ts -list-files`
  lists path/size/pages of the whole corpus; `-all` now works in every
  .djvu-selecting script (handled centrally in corpus.ts selectFiles).
- corpus rework: dropped the local `testfiles/` mirror and the full/subset
  split. `cmd/get-deps.ts` now also clones DjvuNet/artifacts (98 .djvu /
  ~95 MB, blobless + sparse so only the .djvu materialize) and the corpus is
  enumerated in place from the deps/ checkouts by `cmd/corpus.ts` (~115
  files; `DJVU_SPECS=<dir>` overrides). Every script that operates on .djvu
  now does nothing by default — it prints usage + the corpus file count —
  and takes explicit files or `-rand N` (tests.ts/find-slower-pages.ts also
  `-all`). djvudec_stress accepts files as well as dirs. Removed
  dump-features.ts, copy-djvu-files.ts, test/file_features.md.
- fuzz finding (slow-unit-32a1cc…): a crafted Sjbz stream repeating
  StartOfData 77k times made each SOD re-run init_library, which rescanned
  every inherited dict shape bitmap with djvu_bm_bbox — O(records × shapes ×
  pixels), 21s under ASan. jb2_shape now caches its tight bbox (bx0..by1 +
  bbox_valid), mirroring DjVuLibre's JB2Dict::get_bounding_box cache. Thread
  rule: shared-dict shapes are read lock-free by concurrent page decodes, so
  the cache is only written while the owning image is thread-private (during
  its own decode: add_library, plus an end-of-dict-decode fill for shapes
  add_library didn't reach); init_library reads inherited shapes without
  writing. Also a legit perf win: multi-page docs no longer rescan every
  shared-dict shape bitmap per page. Replay: 21.4s → 1.0s. The tracked seed
  (slow-unit-a8cc0f…) is a minimized rebuild of the 136KB fuzz artifact:
  page 3's INFO + inlined dict Djbz + the Sjbz truncated to 20375 bytes —
  23.5KB, and stronger (333k SODs vs 77k; truncation point found by scanning,
  SOD count is non-monotone in Sjbz length). Pre-fix it renders in 5.7s
  release / would hard-timeout the ASan fuzz harness; post-fix 0.3s.
- fuzz finding (timeout-b36526…): the IFF chunk walks clamped a chunk's size
  with `if (cdata + csize > form_end)`, but csize is attacker-controlled and
  the uint32 sum wraps (e.g. csize=0xffffffef advances pos by -8 mod 2^32),
  dodging the clamp and walking the chunk list backwards forever. Rewrote the
  clamp overflow-free (`csize > form_end - cdata`; cdata <= form_end holds
  from the loop bound) in all three walkers: page_load_info,
  djvu_form_find_chunk, and the DJVM DIRM scan in djvu_doc_open.
- fuzz OOM triage (oom-9e1907…): not a leak — a crafted INFO chunk can declare
  a ~1-gigapixel page (25237x36955 @ dpi 6344) and the decoder faithfully
  allocates what the header says (mask + render buffers = multi-GB per input),
  which under ASan quarantine trips libFuzzer's 4GB RSS limit. Verified with
  the tracking allocator: allocs==frees, live=0, no single over-limit malloc.
  Fix in the harness, not the library: fuzz_target.c now installs a budgeted
  allocator (1GB live per input, size stored in a 16-byte block header) so
  oversized header-declared allocations fail cleanly through the library's
  alloc-failure paths — which also puts those error paths under fuzz coverage.
  The biggest legit seed (Mcguffey, peak ~484MB) still decodes fully.
- fuzz finding: JB2 `code_match_index` now bounds-checks the decoded match
  index (crash-06a772…: MatchedRefine before any shape exists →
  `code_num(0,-1)` returns an unclamped value and `lib2shape` is still NULL →
  NULL/OOB read). DjVuLibre is safe here only via GArray's throwing `[]`; the
  C port needs the explicit check. Both call sites bail on `c->error` before
  indexing. Also triaged two slow-unit artifacts: mutations of
  `Mcguffey's_Primer.djvu` whose clean seed already costs ~5s under ASan —
  not bugs; deleted, and `fuzz.ts` now passes `-report_slow_units=20` so the
  big seed's honest cost stops tripping libFuzzer's 10s default.
- subsampled color composite: `djvu_page_render(_into)` now composes
  compound/photo pages at the requested subsample instead of only at full
  resolution (previously subsample>1 fell back to the gray mask on compound
  pages and rendered nothing on photo pages). The background/FG44 layers are
  scaled by the rational ratio red/subsample via the new
  `djvu_cpix_scale_ratio` (for a typical BG44 at red=3 and screen-ish
  subsample 3 the scale is a near-1:1 copy instead of a 3x upsample), and the
  JB2 mask is stenciled anti-aliased by per-cell ink coverage
  (`compose_stencil_sub`, the color counterpart of `render_bitonal`'s coverage
  LUT; palette blits blend their FGbz color, FG44 pages sample the fg at each
  cell center, edge cells use the true in-page cell area). The subsample==1
  path is untouched (byte-exact vs the oracle); `djvu_compose_page(_into)` and
  `djvu_compose_background` take an explicit subsample. Also fixed a latent
  `render_bitonal` mismatch vs ddjvu: coverage cells grouped top-down rows
  while DjVuLibre groups bottom-up (partial cell at the top), which shifted
  reduced bitonal renders by up to sub-1 rows whenever height % sub != 0
  (djvulibre-book-ru at sub 3: mean diff 4.7-9.0/255 -> 0.007-0.016/255).
  Verification: corpus 399/399 MATCH (sub==1 unchanged); `-verify-into` now
  checks subsamples 1-3 per page (render == render_into == render_info on all
  color/bitonal corpus files); new `bun cmd/verify-subsample.ts` compares
  reduced renders against `ddjvu -subsample=N -aspect=no` statistically
  (24 page renders across 4 color files: mean abs diff <= 0.05/255, most
  <= 0.02; NB the ddjvu CLI default shrinks one dim post-ceil for aspect,
  hence -aspect=no). Perf (djvudec_dump -bench-render -sub, uncached, so JB2 +
  IW44 decode dominate): 1998_compression p13 ~94 -> ~58 ms at sub 3
  (iw44+scale 40 -> 26 ms); Mcguffey p31 ~75 -> ~47 ms. The bigger win is on
  the caller side: SumatraPDF's engine had to force compound pages to
  subsample=1 and bilinearly downscale a full-res composite (~10x more pixel
  work + a ~25 MB intermediate RGB for a 300-dpi letter page at screen zoom);
  it can now render color pages at the reduced size directly. `-sub N` added
  to `djvudec_dump -bench-render`.
- investigated `slower-win-msvc.txt` residuals `djvu3spec.djvu` p61 and
  `djvulibre-book-ru.djvu` p26 and found the -bench harness was systematically
  unfair to us in three ways; none of the flagged pages were actually slower:
  1. **ddjvu context cache**: the libdjvu cache lives on the ddjvu *context*
     and survives document close, so in a best-of-2 bench libdjvu's second
     session got page decodes from cache (~0 ms) while ours decoded cold.
     (`ddjvu_cache_set_size(ctx, 0)` is a silent no-op — the API ignores
     sizes <= 0 — so the old "cold" flag never worked either.)
     `bench_ddjvu_reset` now calls `ddjvu_cache_clear`. Proof via new
     `test/jb2prof.cpp` (ref tool, times DjVuLibre's raw JB2 dict/page/render
     phases on chunks extracted with `cmd/extract-chunk.ts`): djvu3spec p61
     Sjbz decode is 10.6 ms in DjVuLibre vs ~2.9 ms ours; ddjvu re-open showed
     create+decode 15.0 ms cold -> 0.02 ms cached.
  2. **phase ordering**: -bench ran both djvudec sessions, then both libdjvu
     sessions; with `find-slower-pages.ts` running 12 files in parallel the
     late-finishing files timed their djvudec phase under heavier ambient load.
     Sessions are now interleaved (ours, lib, ours, lib).
  3. **eager dict decode at open**: `djvu_doc_open` pre-decoded every shared
     JB2 dict (djvu3spec: 4 INCL dicts, 7.8 ms open vs libdjvu's 0.7 ms lazy
     open). Shared dicts now decode lazily on first use when the caller
     supplied lock/unlock hooks (serialized via `djvu_dict_lock`; without
     hooks open still preloads so the dict cache stays immutable for lock-free
     concurrent renders). djvu3spec open 7.4 -> 0.07 ms.
  With the fair bench: djvu3spec p61 17.6 ms libdjvu vs 7.2 ours (-59%),
  book-ru p26 26.3 vs 9.2 (-65%); `slower-win-msvc.txt` regenerated 21 -> 3
  pages, all Mcguffey at noise level (+0.7..+5.9%). Corpus verification
  399/399 MATCH, no leaks.
- generalized the fixed 3x scaler path to ceil-3x sizes: profiled
  `Mcguffey's_Primer.djvu` p3/p7 (2215x3639 pages, BG44 739x1213, red=3); the
  fast red-3 path required both output dims to be exact 3x multiples, and
  2215 = 3*739-2 fell back to the generic per-pixel bilinear (~13 ms vs ~7.7 ms
  for the fast path). Since `prepare_coord(red=3)` for a ceil-3x output is the
  exact 3x coordinate sequence truncated to outw/outh (trailing samples clamp
  to the last input pixel/row), the fast path now accepts
  `outw in [3w-2, 3w]` / `outh in [3h-2, 3h]` (guarded on the actual red=3
  ratio, stored in the scaler) and emits the clamped tail replicas explicitly.
  p3 33.3 -> 27.4 ms, p7 39.2 -> 32.4 ms (-17%); `bun cmd/bench.ts` now shows
  every Mcguffey page ~25% faster than libdjvu (p3 44.3 libdjvu vs 32.1 ours;
  p7 52.0 vs 38.9, previously +40%/+35% slower). Renders byte-identical to the
  generic path; MSVC corpus verification 399/399 MATCH.
- render-speed pass for 3x IW44 background scaling: profiled
  `1998_lossy_masked.djvu` p6/p10 and found the hot path in the GPixmapScaler
  expansion of BG44 `852x1100` to page `2556x3300`. Added a byte-exact fixed
  3x scaler path that keeps the same vertical-then-horizontal rounding order as
  the generic scaler. Target pages now benchmark faster than libdjvu (p6
  46.62 ms libdjvu vs 34.36 ms ours; p10 47.72 ms vs 32.77 ms), target
  `-verify-into` and clang corpus verification stayed byte-exact, and
  `slower-mac-clang.txt` regenerated with 0 slower pages.
- render-speed pass for sparse JB2 direct bitmaps: profiled `djvu3spec.djvu`
  p62 with Xcode Time Profiler (`xctrace`) plus `sample`; the hot path was
  direct JB2 bitmap decode of large mostly-white 300x300 page-local tiles. Added
  a ZP context-0 white-run fast path, row-run bitonal stamping, and selective
  page-local shape compression (keep single-use shapes as bytes). Final
  `bun cmd/bench.ts testfiles/djvu/djvu3spec.djvu`: p62 5.58 ms libdjvu vs
  4.00 ms ours, with p61-p64 all faster than libdjvu; clang corpus verification
  and `-verify-into` stayed byte-exact.
- render-speed pass for palette compound pages: added `cmd/profile-render.ts`
  for macOS `sample`/Xcode Time Profiler runs, a direct top-down RGB compositor
  for identity-gamma `FGbz` pages, a caller-buffer scaler path, cached scaler
  horizontal coordinates, and faster GBitmap RLE encoding via `memchr`. Target
  page `djvulibre-book-ru.djvu` p39 went from slower than libdjvu to slightly
  faster in `bun cmd/bench.ts`; clang corpus verification stayed byte-exact.
- richer public API (modeled on SumatraPDF's ddjvuapi usage): structured text
  with bounding boxes (zone tree, src/text.c), document outline/bookmarks
  (NAVM, src/outline.c), page hyperlinks/annotations (ANTa/ANTz maparea,
  src/annot.c), page id/title + named-destination resolution, and page type
  (bitonal/photo/compound). Verified vs djvutxt --detail / djvused
  print-outline / djvused print-ant (coords byte-exact).
- merged per-module headers (bitmap/bzz/iw44/jb2/zp) into one src/djvu_internal.h
  (no functional change; verification unchanged).
- composite (mask+bg+fg) + GPixmapScaler: 188/189 pages == ddjvu (1 mask edge case).
- IW44 wavelet decoder (BG44/FG44): 26/26 color images == DjVuLibre IW44Image.
- text extraction (TXTz/TXTa): 144/144 text pages == djvutxt content.
- JB2 bitonal decoder + GBitmap + render: 122/122 pure-mask pages == ddjvu.
- full DJVM/DIRM directory parse (BZZ component table); INCL resolution.
- BZZ decompressor (round-trips vs `bzz -e`).
- scaffold, ref tools built, ZP table extracted, milestone 1 (page info).
