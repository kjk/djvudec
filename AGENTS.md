# AGENTS.md — working on the C DjVu decoder

Plain-C, decode-only DjVu library, ported from the C# **DjvuNet** project and
verified byte-for-byte against **DjVuLibre**. API style follows **jbig2dec**.

## Goal / scope
Decode-only. The caller hands us the entire `.djvu` file up-front as an
in-memory buffer (no incremental fetch). We provide exactly three things:
- enumerate pages + report page info (dimensions, dpi, rotation, version)
- produce a decompressed bitmap for a page (bitonal / gray / color)
- extract page text

No encoders. No writing DjVu.

**XML is intentionally not ported.** DjVuLibre's XML code (`xmltools/`,
`libdjvu/XMLParser.*`, `XMLTags.*`; the `djvutoxml`/`djvuxmlparser` tools) is a
round-trip *serialize/edit* format, not part of the decode path — reading a
`.djvu` produces internal structs that DjVuLibre exposes as s-expressions
(miniexp), with XML a separate optional export plus an *encode-back* importer.
It's out of scope on two counts: it's an encoder, and the structured data it
would carry (outline, hidden-text zones, hyperlinks) we already expose as typed
C structs (`djvu_doc_outline`, `djvu_page_text_get_zones`, `djvu_page_get_links`)
that a caller can serialize themselves. The C# port (DjvuNet) has no XML code
either. Don't re-investigate this.

## Reference checkouts (local)
- C# source being ported:  `C:\Users\kjk\src\DjvuNet\DjvuNet`  (DjvuNet repo)
- Verification oracle:      `C:\Users\kjk\src\DjVuLibre`        (DjVuLibre repo)
- `bun cmd/get-deps.ts` clones both repos as siblings of this project (skipped
  if present) and assembles the test corpus into `testfiles/djvu/*.djvu` by
  copying every `.djvu` from `DjVuLibre/doc`, `DjvuNet/Specs`, and
  `DjvuNet/DjvuNetTest/TestFiles`. Exported as `getDeps()`; `build.ts` and
  `verify.ts` both call it, so a fresh checkout self-provisions. `testfiles/`
  is gitignored.
- Spec: https://www.sndjvu.org/spec.html
  (the **code** — DjvuNet and especially DjVuLibre — is the more definitive
  reference; the spec text is incomplete.)

Real-world corpora used for stress testing: `Z:\sumtest` (36 files),
`Z:\backup\books` (1396 files).

## Build & test
- `bun cmd/build.ts` — fetches deps (`getDeps`), builds the DjVuLibre reference
  tools **once** into `ref_build/`, then compiles the C library + test harness
  with clang (`-std=c11`) into `djvu_test.exe`. `bun cmd/build.ts ref` rebuilds
  just the ref tools. `buildRef()`/`build()` are exported for `verify.ts`.
  djvu_test also links DjVuLibre's decoder (via the `test/bench_ddjvu.cpp` shim
  and a cached `ref_build/libdjvu.lib`, built once by `buildLibDjvu()`) so that
  `-bench` works. NB: Bun's shell eats `\` in glob args, so `ROOT` is normalized
  to forward slashes — keep paths fed to `*.cpp`/`*.o` globs forward-slashed.
- `bun cmd/bench.ts [file.djvu]` — builds, then runs `djvu_test -bench` to
  compare our per-page decode speed against DjVuLibre's (decode+composite, same
  steady_clock both sides). With no file it picks a random `.djvu` from
  `testfiles/`. Each line: `page N, djvulibre A ms, ours B ms, +/-Δ ms, +/-Δ%`
  (`+` = we're slower).
- `bun cmd/verify.ts` — the **test driver**: ensures deps, calls
  `buildRef()`+`build()` from `build.ts` (build first, then verify), and
  compares against the oracle over `testfiles/djvu/*.djvu`. (This inverts the
  old relationship where build.ts invoked verify.)
- IMPORTANT: run these from the `djvu` dir. The ref-tool build
  `cd`s into the DjVuLibre dir; if cwd is left there you get "Module not found".
- Reference tools are built static from `libdjvu/*.cpp` with
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT -ladvapi32`.
  `djvudump` crashes in this clang build and is not used; `ddjvu`, `djvutxt`,
  `bzz`, `djvused` are sufficient. Oracles for the structured APIs:
  `djvutxt --detail=word` (text zones), `djvused -e 'print-outline'` (outline),
  `djvused -e 'select N; print-ant'` (hyperlinks; inject test annotations with
  `set-ant` via `djvused in.djvu -f script.dsed`).

### Verification scripts
- `bun cmd/verify.ts` — corpus verifier (builds first). mask→pgm, bg/color→ppm,
  plus text. Scans every `.djvu` under `testfiles/` **recursively**; set the
  `DJVU_SPECS` env var to point the scan at any other directory (e.g. a
  real-world set) instead.
- The old Python verifiers (`test/verify.py`, `test/verify_dir.py`) have been
  removed; `cmd/verify.ts` is their bun/TypeScript replacement.

The single render MISMATCH is `1998_compression.djvu` p19 — NOT a decode bug.
It is a ddjvu three-layer-stencil quirk (ddjvu paints a few FG pixels ~1px off
from the JB2 mask). Verified our mask/bg/fg are byte-exact vs DjVuLibre
internals. Our output is arguably more correct. Do not "fix" it.

### Amalgamation (single-file distribution)
- `bun cmd/build-dist.ts` — generates an SQLite-style amalgamation in `dist/`:
  `dist/djvu.h` (verbatim public header) and `dist/djvu.c` (the public header +
  `djvu_internal.h` + every `src/*.c` concatenated into one translation unit,
  with the local `#include "djvu.h"` / `"djvu_internal.h"` lines stripped). The
  script verifies the result with `clang -c` before finishing. `dist/*.c/.h`
  are committed (regenerate after touching `src/`); the build
  artifacts are gitignored. This works because no two `.c` files share a
  file-local (`static`) symbol name — keep it that way or the single-unit build
  breaks.

### Helper tests / scripts
Write one-off helper tests, probes, and verification scripts in TypeScript and
run them with **bun**, placed in the **`cmd/`** directory (e.g.
`bun cmd/<name>.ts`). Prefer this over throwaway Python or shell scripts —
bun + TypeScript is the standard tooling for ad-hoc tooling here.

### Windows gotchas
- `test/djvu_test.c` uses `fopen` → ASCII paths only. The **library** takes an
  in-memory buffer and is encoding-agnostic; only the test harness is limited.
  Unicode-path "failures" in the wild are harness limitations, not bugs.
- Python on Windows can't read git-bash `/tmp` paths; write test outputs to the
  session scratchpad dir instead.

## C API (`src/djvu.h`) — jbig2dec flavor
Opaque `djvu_ctx` / `djvu_doc`; caller-supplied `djvu_alloc_cb` / `free_cb` /
`error_cb`. Key calls: `djvu_doc_open(ctx,data,len)`, `djvu_doc_page_count`,
`djvu_doc_page_info(doc,page,&info)`, `djvu_page_render(doc,page,subsample)`
→ `djvu_image{width,height,format(GRAY8=1/RGB24=3),stride,data}`,
`djvu_page_text`, plus destroy functions.

Richer accessors (added to match SumatraPDF's ddjvuapi usage in
src/EngineDjVu.cpp): `djvu_page_get_type` (bitonal/photo/compound);
`djvu_doc_page_id` / `_title` / `_by_name` (DIRM labels + named-destination
resolution); `djvu_page_text_get_zones` (hidden-text zone tree with top-down
bounding boxes); `djvu_doc_outline` (NAVM bookmarks, synthetic root); 
`djvu_page_get_links` (ANTa/ANTz maparea hyperlinks). All coordinates are
full-resolution, top-down (origin top-left), matching render output at
subsample=1. Test harness flags: `-type -zones -outline -links`.

## Internal headers
All internal declarations live in a **single** `src/djvu_internal.h` (one
labeled section per module: core, zpcodec, bitmap, bzz, jb2, iw44). Every `.c`
file includes just that one header. (Previously these were five separate
headers: djvu_bitmap.h / djvu_bzz.h / djvu_iw44.h / djvu_jb2.h / djvu_zp.h.)

## C → C# source map
Paths relative to `C:\Users\kjk\src\DjvuNet\DjvuNet\`.

| C file (`src/`) | C# source(s) (`DjvuNet/`) |
|---|---|
| `document.c` | `Parser/DjvuParser.cs`, `DjvuDocument.cs`; chunks `DataChunks/DjvmChunk.cs`, `DirmChunk.cs`, `InfoChunk.cs`, `DjvuChunk.cs`, `DjviChunk.cs`, `InclChunk.cs` |
| `djvu_internal.h` (byte readers) | `IO/DjvuReader.cs` |
| `zptable.c` | `Compression/ZPTable.cs` (+ default table block in `ZPCodec.cs`) |
| `zpcodec.c` | `Compression/ZPCodec.cs` (decode path only) |
| `bzz.c` | `Compression/BSInputStream.cs`, `BSBaseStream.cs`; decode side of `BzzReader.cs` |
| `bitmap.c` | `Graphics/Bitmap.cs` (GBitmap) |
| `jb2.c` | `JB2/JB2Decoder.cs`, `JB2Codec.cs`, `JB2Image.cs`, `JB2Dictionary.cs`, `JB2Shape.cs`, `JB2Blit.cs` |
| `iw44.c` | `Wavelet/InterWavePixelMapDecoder.cs`, `InterWaveMapDecoder.cs`, `InterWaveCodec.cs`, `InterWaveMap.cs`, `InterWaveBlock.cs`, `InterWaveBucket.cs`, `InterWaveImage.cs`; transform from `Wavelet/InterWaveTransform.cs` |
| `iw44_zigzag.c` | zigzag/quant tables from `Wavelet/InterWaveCodec.cs` / `InterWaveMap.cs` |
| `text.c` | `Text/PageText.cs`, `PageTextItem.cs`; chunks `DataChunks/TxtzChunk.cs`, `TxtaChunk.cs`; zone tree from `DataChunks/Text/TextChunk.cs`, `TextZone.cs` |
| `outline.c` | `DataChunks/NavmChunk.cs`, `Navigation/Bookmark.cs` |
| `annot.c` | annotation chunks (`DataChunks/AntaChunk.cs`, `AntzChunk.cs`); maparea S-expr parsing (DjVuLibre `ddjvu_anno_get_hyperlinks` equivalent) |
| `compose.c` | `DjvuImage.cs` (composite/`GetPixmap`) + `Graphics/PixelMapScaler.cs` (`GPixmapScaler`); pixel ops from `Graphics/PixelMap.cs`, `Pixel.cs` |
| `render.c` | `DjvuPage.cs` (page orchestration: mask + bg + fg layer selection) |

Notes:
- `DataChunks/*Chunk.cs` are thin wrappers in C#; in the C port their logic is
  inlined into the chunk dispatch in `document.c`/`render.c`/`compose.c`, not
  separate files.
- Decode-only: all C# encoder counterparts (`BSOutputStream.cs`,
  `JB2Encoder.cs`, `InterWave*Encoder.cs`, `BzzWriter.cs`, …) have no C
  equivalent.
- For the wavelet transform I follow DjVuLibre's `filter_bv`/`filter_bh`
  verbatim (matches DjvuNet's scalar `Unified` path; ignore the C#
  `Vector128`/`Vector256` SIMD variants).

## Format / codec cheat-sheet
- **Container**: IFF/AT&T FORM chunks. `FORM:DJVU` (single page),
  `FORM:DJVM` (bundled multipage with `DIRM` directory), `FORM:DJVI`
  (shared include/dict). DIRM directory is BZZ-compressed; component types
  0=incl(DJVI), 1=page, 2=thumb, 3=anno.
- **INFO chunk**: width/height u16-BE, DPI u16-LE, gamma = byte/10, rotation
  flag.
- **ZP-coder**: binary adaptive arithmetic decoder. Default table extracted
  from the C# `#if !ZCODER` block (lines ~887-1144 of ZPCodec.cs) — NOT the
  other `#ifdef` block. zptable.c has 256 entries (0-250 real, 251-255 zero
  padding).
- **BZZ**: Burrows-Wheeler + ZP. MaxBlock=4096, FreqMax=4, CTXIDS=3.
  Effective block length = size-1 (marker excluded).
- **JB2**: bitonal. Adaptive number-coder tree; direct + cross (refinement)
  bitmap coding; shared dictionary (Djbz) in-page or via INCL→DJVI.
  `ResetNumcoder` (RequiredDictOrReset) resets the number-coder mid-stream for
  large dicts — must be implemented (some files have empty pages otherwise).
- **GBitmap**: stored **bottom-up**. bytes_per_row=width+border,
  max_offset=height*bytes_per_row+border, GetByteAt returns 0 out-of-range.
- **IW44**: wavelet. bands/buckets/bitplanes, sparse 64-bucket blocks, zigzag,
  inverse transform (filter_bv/filter_bh from DjVuLibre), YCbCr→RGB.
  Multi-chunk progressive refinement; crcb delay/half. iw_quant table is 4×
  DjVuLibre's, compensated by an initial
  `while(quant_low[0]>=32768) NextQuant()` pre-shift.
  **CRITICAL**: IW44/GBitmap are bottom-up; DjVuLibre `save_ppm` writes
  bottom-up (= natural top-down output). My render must FLIP to top-down.
  (1% luma differences usually mean a missing flip, not gamma.)
- **Composite** (`DjvuImage::get_pixmap`): background (IW44, bilinear-upsampled
  via GPixmapScaler) + foreground (FGbz palette two-layer, or FG44 three-layer
  nearest-upsample) stenciled through the JB2 mask. At subsample=1 the mask is a
  hard binary stencil. Color composites only at subsample==1 currently;
  subsample>1 on a color page falls back to the gray mask (TODO: scale).
- **Gamma**: corr = target_gamma(2.2) / document_gamma (INFO gamma). LUT
  out=255·(in/255)^(1/corr), endpoints forced 0/255. Applied to color
  composites only (bitonal unaffected). Matches ddjvu.
- **Rotation**: INFO flag → 90/180/270. ddjvu rotates output upright (dims swap
  for 90/270). My mapping: 90→k=3, 180→k=2, 270→k=1 clockwise quarter-turns,
  applied at end of render.
- **Blank pages**: INFO-only components render as solid white at INFO dims.
- **Pure-photo pages** (BG44-only, no mask): composite with NULL mask
  (background only).

## Debug hooks (env vars)
- `DJVU_NOCOMPOSE` — bypass the color composite.
- `DJVU_JB2_DEBUG` — per-stream JB2 record-type histogram.
- `DJVU_JB2_BLITS` — dump blit positions/sizes.
- `DJVU_JB2_SHAPE=N` — ASCII dump of shape N.
- `DJVU_IW_DEBUG`, `DJVU_IW_MAXCHUNKS` — IW44 tracing / chunk limiting.

## Per-layer reference tools (distinguish my-bug vs ddjvu-quirk)
- `test/iw44ref.cpp` — decode raw IW44 (PM44) via DjVuLibre IW44Image internals.
- `test/jb2ref.cpp` — decode raw Sjbz, dump blits/mask via DjVuLibre internals.
- `djvu_test.exe` flags: `-info -page N -out f -text -bzzdec -comps -bench`,
  IW44 debug: `-iwbg/-iwfg/-iwdumpbg/-iwdumpfg/-iwbggray/-iwbgcb/-iwbgcr -bg`.
  `-bench` times our decode vs DjVuLibre's per page (see `bun cmd/bench.ts`).

## Methodology
Reference-oracle verification: every codec layer is checked byte-exact against
DjVuLibre internals, isolating my-decode bugs from ddjvu rendering quirks. Work
incrementally and keep `PROGRESS.md` current. See `PROGRESS.md` for the
milestone history and change log.

**Do not commit automatically.** Make and verify changes, but leave them staged
in the working tree; only run `git commit` when the user explicitly asks. (The
user reviews diffs and decides when to commit.)

## Status
Feature-complete; verified byte-for-byte vs DjVuLibre. All remaining
non-matches are the cosmetic ddjvu fg-stencil quirk on a handful of pages.
