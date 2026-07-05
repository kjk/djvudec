# djvu — C port of DjVu decoding (DjvuNet → C, jbig2dec style)

Goal: a plain-C library that decodes DjVu files enough to:
- enumerate pages + report page info (dimensions, dpi, rotation)
- produce a decompressed bitmap for a page (bitonal, gray, color)
- extract page text

We are given the whole file up-front (no incremental fetch). Decode only — no encoders.

## References (checked out locally)
- C# source being ported:   `deps/DjvuNet/DjvuNet`            (DjvuNet repo)
- Verification oracle:      `deps/DjVuLibre`                  (DjVuLibre repo)
- Test files (11 .djvu):    `testfiles/djvunet/*.djvu` (copied from DjvuNet/Specs; gitignored)
- Spec: https://www.sndjvu.org/spec.html  (code is the more definitive reference)

## Reference tools (built from DjVuLibre, see cmd/build.ts `build_ref()`)
Built into `ref_build/`:
- `ddjvu.exe`    — `ddjvu -format=pgm -page=N in.djvu out.pgm`  (bitmap oracle)
- `djvutxt.exe`  — `djvutxt --page=N in.djvu`                   (text oracle)
(djvudump crashes in this build; not used.)

## Test corpus survey (via ddjvu/djvutxt)
All 11 files are DJVM (multi-page bundled, BZZ-compressed DIRM directory).
- bitonal (P4 / JB2): 9 files
- color   (P6 / IW44): 1998_compression.djvu, 1998_lossy_masked.djvu
- 7 files have hidden text (BZZ + Txta/Txtz)

## C API (src/djvu.h) — jbig2dec flavor
Opaque ctx/doc/page; caller-supplied alloc/free/error callbacks. See header.

Internal headers are consolidated into a single `src/djvu_internal.h` (one
labeled section per module: core, zpcodec, bitmap, bzz, jb2, iw44). Every `.c`
file includes just that one header.

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
| compose.c           | DjvuImage composite + GPixmapScaler     | DONE   |
| render/compose      | DjvuPage composite (mask+fg+bg)         | DONE   |

## Status: feature-complete; verified byte-for-byte vs DjVuLibre
`bun cmd/tests.ts` (every .djvu under testfiles/, recursively):
  render (mask=pgm, bg/color=ppm): MATCH=398 MISMATCH=1; text: MATCH=353.
`DJVU_SPECS=<dir> bun cmd/tests.ts` (point at any directory of .djvu) on
real-world sets (tests.ts replaced the old Python verify_dir.py):
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
  exactly with the mask. (debug hooks: DJVU_NOCOMPOSE, DJVU_JB2_BLITS,
  DJVU_JB2_SHAPE=N; reference tools test/jb2ref.cpp, test/iw44ref.cpp.)
- Color pages composite only at subsample==1 (full res); subsample>1 on a
  color page currently falls back to the gray mask. TODO: scale composite.

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
6. Page scaling / subsample to requested dimensions (basic box subsample done).

## Notes for next session (IW44)
- Dict resolution: in-page `Djbz` takes precedence; else `INCL` id -> external
  DJVI component (djvu_doc_component_offset) -> its `Djbz`. (render.c)
- Pages needing IW44: have `BG44` chunks (background, possibly multiple = IW44
  refinement chunks to be decoded in sequence) and `FGbz` (FG color palette);
  composite = background pixmap, then foreground colors through the JB2 mask.
- `ddjvu -format=pgm` gives the gray composite; `-format=ppm` gives color.
  Color test files (P6): 1998_compression, 1998_lossy_masked.
- C# source: Wavelet/InterWave*Decoder.cs (IW44Image / InterWavePixelMap).
- JB2 codec entry: djvu_jb2_decode (Sjbz/image), djvu_jb2_decode_dict (Djbz).
- DJVU_JB2_DEBUG=1 env prints a per-stream record-type histogram.

## Build / test
`bun cmd/get_deps.ts` — clones DjvuNet + DjVuLibre into deps/, assembles testfiles/djvu.
`bun cmd/build.ts` — builds ref tools (once), the C library + test harness with clang.
`bun cmd/tests.ts` — the test driver: ensures deps, builds, then verifies over
every .djvu under testfiles/ (recursively).

## Known unported chunks / decode gaps
Decode-path features present in DjvuNet (C#) that this port does NOT handle yet.
Verified against `src/*.c` chunk dispatch. We DO handle: INFO, DIRM, NAVM, INCL,
Sjbz, Djbz, BG44, FG44, FGbz, PM44, TXTz, TXTa, ANTa, ANTz.

- **`BGjp` / `FGjp` — JPEG-coded bg/fg layers.** No JPEG decoder in the C port;
  a page whose background/foreground is JPEG (instead of IW44) renders blank.
  Rare. The only real correctness gap besides Smmr; would need a JPEG decoder.
- **`Smmr` — CCITT-G4/MMR-coded bitonal mask** (legacy alternative to JB2 Sjbz).
  Not handled; such pages get no mask. Rare.
- **`TH44` / `FORM:THUM` — embedded thumbnails.** Not decoded or exposed (no
  thumbnail API). Pure convenience: full-page render already works. C# exposes
  `DjvuPage.Thumbnail`.
- **`BM44` — standalone grayscale wavelet form.** Only relevant to raw
  standalone wavelet files, not normal pages (page bg uses BG44). Not handled.
- **`Wmrm` (watermark-removal), `CIDA` (obsolete).** Not handled; negligible,
  normally ignored even by DjVuLibre.
- **Annotation richness (partial).** We extract hyperlink mapareas
  (rect/oval/text/poly/line + url + comment); we do NOT model highlight color,
  border styles, pushpins, etc. that C#'s `Annotation` carries.

Deliberately OUT of scope (not gaps): all encoders/writers (no DjVu writing),
external image-format I/O (ImageConverter, PBM/PGM/RLE serialization), and
progressive/incremental decoding (we decode the whole buffer up-front). SIMD
wavelet paths, caching, and multithreading are impl details, not features.
NB: we render INFO rotation (compose.c); the C# port does not.

## Change log (most recent first)
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
     phases on chunks extracted with `cmd/extract_chunk.ts`): djvu3spec p61
     Sjbz decode is 10.6 ms in DjVuLibre vs ~2.9 ms ours; ddjvu re-open showed
     create+decode 15.0 ms cold -> 0.02 ms cached.
  2. **phase ordering**: -bench ran both djvudec sessions, then both libdjvu
     sessions; with `find_slower_pages.ts` running 12 files in parallel the
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
- render-speed pass for palette compound pages: added `cmd/profile_render.ts`
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
