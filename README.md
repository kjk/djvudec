# djvu — a plain-C DjVu decoder

A from-scratch C port of the [DjvuNet](https://github.com/DjvuNet/DjvuNet) (C#)
DjVu decoder, in the style of [jbig2dec](https://github.com/ArtifexSoftware/jbig2dec).
Decode only: page info, bitmaps, and hidden text. The whole file is supplied
up-front as an in-memory buffer.

Correctness is verified against [DjVuLibre](https://github.com/DjvuNet/DjVuLibre)
(`ddjvu`, `djvutxt`) on the sample files in `DjvuNet/Specs`.

## Status
| Feature | State |
|---|---|
| Page count / dimensions / dpi / rotation | ✅ matches ddjvu (11/11 files) |
| Bitonal page bitmap (JB2) | ✅ byte-exact vs ddjvu (122/122 mask pages) |
| Hidden text (TXTz/TXTa) | ✅ matches djvutxt (144/144 pages) |
| Color / gray pages (IW44 + composite) | ✅ byte-exact vs ddjvu (188/189 pages) |

Overall: `bun cmd/tests.ts` → render 188/189 == ddjvu, text 144/144 ==
djvutxt. The 1 render miss (1998_compression p19, 0.008% of pixels) is a ddjvu
three-layer-stencil quirk, not a decode bug: our JB2 mask, background, and FG44
each match DjVuLibre's internals byte-for-byte; ddjvu just paints the foreground
~1px off the mask for a few shapes there. See PROGRESS.md.

See [PROGRESS.md](PROGRESS.md) for the design, port map, and milestones.

## API
See [`src/djvu.h`](src/djvu.h). Sketch:
```c
djvu_ctx *ctx = djvu_ctx_new(NULL, NULL, on_error, NULL);
djvu_doc *doc = djvu_doc_open(ctx, data, len);     /* data must outlive doc */
int n = djvu_doc_page_count(doc);
djvu_page_info info; djvu_doc_page_info(doc, 0, &info);
djvu_image *img = djvu_page_render(doc, 0, 1);     /* gray8 / rgb24 */
char *txt = djvu_page_text(doc, 0);
```

## Build & test
Requires `clang`, `bun`, and `git`. `cmd/get-deps.ts` clones the DjvuNet and
DjVuLibre repos as siblings of this project and assembles the test corpus into
`testfiles/djvu/`; `build.ts` and `tests.ts` call it automatically.
```
bun cmd/get-deps.ts     # clone deps + assemble testfiles/djvu (auto-run below)
bun cmd/build.ts        # build reference tools + the C library and djvu_test.exe
bun cmd/tests.ts        # build, then verify render + text against DjVuLibre
```

`djvu_test` CLI (jbig2dec-flavored):
```
djvu_test -info in.djvu
djvu_test -page N -out out.pgm in.djvu
djvu_test -page N -text in.djvu
```
