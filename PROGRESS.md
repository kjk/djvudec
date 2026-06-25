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
| bytestream.c        | IO/DjvuReader.cs (BE/LE memory reader)  | DONE   |
| zptable.c           | Compression/ZPCodec.cs default table    | DONE   |
| zpcodec.c           | Compression/ZPCodec.cs (decode only)    | DONE   |
| iff.c / document.c  | Parser + DataChunks (DJVM/DIRM/INFO)    | WIP    |
| bzz.c               | Compression/BSInputStream + BzzReader   | TODO   |
| jb2.c               | JB2/* (decode)                          | TODO   |
| iw44.c              | Wavelet/* (decode)                      | TODO   |
| bitmap.c / pixmap.c | Graphics/Bitmap, PixelMap               | TODO   |
| text.c              | Text/PageText + Txta/Txtz chunks        | TODO   |
| render/compose      | DjvuPage composite (mask+fg+bg)         | TODO   |

## Milestones
1. **Page info** (no codecs): DJVM/DIRM offset scan + INFO chunk → page count + dims.
   Verify against `ddjvu` dims. ← current
2. **ZP + JB2** → bitonal page bitmap; verify vs `ddjvu -format=pgm`.
3. **BZZ** → DIRM names (INCL resolution), text chunks.
4. **Text extraction**; verify vs `djvutxt`.
5. **IW44** → color page; full composite; verify vs `ddjvu`.
6. Page scaling / subsample to requested dimensions.

## Build / test
`bun build.ts` — builds ref tools (once), the C library + test harness with clang.
`bun build.ts test` — runs verification over Specs/*.djvu.

## Change log (most recent first)
- (init) scaffold, ref tools built, ZP table extracted, milestone 1 WIP.
