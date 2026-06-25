# djvu — C port of DjVu decoding (DjvuNet → C, jbig2dec style)

Goal: a plain-C library that decodes DjVu files enough to:
- enumerate pages + report page info (dimensions, dpi, rotation)
- produce a decompressed bitmap for a page (bitonal, gray, color)
- extract page text

We are given the whole file up-front (no incremental fetch). Decode only — no encoders.

## References (checked out locally)
- C# source being ported:   `../../DjvuNet/DjvuNet`            (DjvuNet repo)
- Verification oracle:      `../../DjVuLibre`                  (DjVuLibre repo)
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
`bun cmd/verify.ts` (every .djvu under testfiles/, recursively):
  render (mask=pgm, bg/color=ppm): MATCH=398 MISMATCH=1; text: MATCH=353.
`DJVU_SPECS=<dir> bun cmd/verify.ts` (point at any directory of .djvu) on
real-world sets (verify.ts replaced the old Python verify_dir.py):
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
   (`bun cmd/verify.ts`)
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
`bun cmd/get-deps.ts` — clones DjvuNet + DjVuLibre siblings, assembles testfiles/djvu.
`bun cmd/build.ts` — builds ref tools (once), the C library + test harness with clang.
`bun cmd/verify.ts` — the test driver: ensures deps, builds, then verifies over
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
