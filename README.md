# djvudec — a plain-C DjVu decoder

This is a djvu format decoder, like [libdjvu](https://github.com/barak/djvulibre), but better:
* smaller (in [SumatraPDF](https://www.sumatrapdfreader.org/), I saved 400 kB when I replaced libdjvu)
* plain C, no dependencies (vs. C++)
* simpler API
* simpler to integrate: copy dist/djvu.h and dist/djvu.c into your project. This is amalgamated src/* into a single file, like sqlite
* mostly faster, according to benchmarks (`bun cmd/bench.ts`)
* tested on thousands of .djvu files for bit-by-bit correctness to libdjvu

## API
See [`src/djvu.h`](src/djvu.h). Sketch:
```c
djvu_init();                                       /* once, before threads */
djvu_ctx *ctx = djvu_ctx_new(NULL, NULL, on_error, NULL);
djvu_doc *doc = djvu_doc_open(ctx, data, len);     /* data must outlive doc */
int n = djvu_doc_page_count(doc);
djvu_page_info info; djvu_doc_page_info(doc, 0, &info);
djvu_image *img = djvu_page_render(doc, 0, 1);     /* gray8 / rgb24 */
char *txt = djvu_page_text(doc, 0);
```

## Build & test
Requires `clang`, `bun`, and `git`. `cmd/get-deps.ts` clones the DjvuNet and
DjVuLibre repos into `deps/` and assembles the test corpus into
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

## How it was made

This is automatic, ai assited port of of [DjvuNet](https://github.com/DjvuNet/DjvuNet) (C#)
DjVu decoder, done with Grok Build.

Correctness is verified against [DjVuLibre](https://github.com/DjvuNet/DjVuLibre)
(`ddjvu`, `djvutxt`) on the sample files in `deps/DjvuNet/Specs`, and my own collection large collection of djvu files collected for testing [SumatraPDF](https://www.sumatrapdfreader.org/).
