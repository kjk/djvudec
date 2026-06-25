# djvu — C port of DjVu decoding (DjvuNet → C, jbig2dec style)

Goal: a plain-C library that decodes DjVu files enough to:
- enumerate pages + report page info (dimensions, dpi, rotation)
- produce a decompressed bitmap for a page (bitonal, gray, color)
- extract page text

We are given the whole file up-front (no incremental fetch). Decode only — no encoders.

## References (checked out locally)
- C# source being ported:   `../../DjvuNet/DjvuNet`            (DjvuNet repo)
- Verification oracle:      `../../DjVuLibre`                  (DjVuLibre repo)
- Test files (11 .djvu):    `../../DjvuNet/Specs/*.djvu`
- Spec: https://www.sndjvu.org/spec.html  (code is the more definitive reference)

## Reference tools (built from DjVuLibre, see build.ts `build_ref()`)
Built into `ref_build/`:
- `ddjvu.exe`    — `ddjvu -format=pgm -page=N in.djvu out.pgm`  (bitmap oracle)
- `djvutxt.exe`  — `djvutxt --page=N in.djvu`                   (text oracle)
(djvudump crashes in this build; not used.)

## Test corpus survey (via ddjvu/djvutxt)
All 11 files are DJVM (multi-page bundled, BZZ-compressed DIRM directory).
- bitonal (P4 / JB2): 9 files
- color   (P6 / IW44): 1998_compression.djvu, 1998_lossy_masked.djvu
- 7 files have hidden text (BZZ + Txta/Txtz)

## C API (include/djvu.h) — jbig2dec flavor
Opaque ctx/doc/page; caller-supplied alloc/free/error callbacks. See header.

## Architecture / port map
| C module            | from C# (DjvuNet)                       | status |
|---------------------|-----------------------------------------|--------|
| (readers inline)    | IO/DjvuReader.cs (BE/LE in djvu_internal)| DONE   |
| zptable.c           | Compression/ZPCodec.cs default table    | DONE   |
| zpcodec.c           | Compression/ZPCodec.cs (decode only)    | DONE   |
| document.c          | Parser + DataChunks (DJVM/DIRM/INFO)    | DONE   |
| bzz.c               | Compression/BSInputStream (decode)      | DONE   |
| jb2.c               | JB2/* (decode)                          | TODO   |
| iw44.c              | Wavelet/* (decode)                      | TODO   |
| bitmap.c / pixmap.c | Graphics/Bitmap, PixelMap               | TODO   |
| text.c              | Text/PageText + Txta/Txtz chunks        | TODO   |
| render/compose      | DjvuPage composite (mask+fg+bg)         | TODO   |

## Milestones
1. **Page info** (no codecs): DJVM/DIRM + INFO → page count + dims. ✅ DONE
   (all 11 Specs files match ddjvu dims)
2. **BZZ** decompressor. ✅ DONE (round-trips vs `bzz -e`; decodes real DIRM)
   - full DIRM parse: component ids/types resolved (INCL resolution ready)
3. **ZP + JB2** → bitonal page bitmap; verify vs `ddjvu -format=pgm`. ← NEXT
   (9/11 files are bitonal; pages reference a shared Djbz dict via INCL→DJVI)
4. **Text extraction** (Txta/Txtz, BZZ); verify vs `djvutxt`.
5. **IW44** → color page; full composite; verify vs `ddjvu`.
6. Page scaling / subsample to requested dimensions.

## Notes for next session (JB2)
- Page FORM:DJVU layout: INFO, INCL (-> "dict0006.iff" shared dict), Sjbz (the
  JB2 bitonal mask), optional FG44/BG44 (color), FGbz (palette), Txta/Txtz.
- INCL id resolves via djvu_doc_component_offset(); shared dict is a FORM:DJVI
  containing a Djbz chunk (JB2 dictionary, no image).
- Sjbz + Djbz both decode with the JB2 codec (JB2/JB2Codec.cs). JB2 uses the ZP
  coder directly (djvu_zp_decode with NumContext context arrays).
- For bitonal pages the composite output == the JB2 mask rendered 0/255 gray;
  compare against `ddjvu -format=pgm -page=N`.
- C# entry points: JB2Image.Decode / JB2Dictionary; JB2Codec.cs is the core.

## Build / test
`bun build.ts` — builds ref tools (once), the C library + test harness with clang.
`bun build.ts test` — runs verification over Specs/*.djvu.

## Change log (most recent first)
- (init) scaffold, ref tools built, ZP table extracted, milestone 1 WIP.
