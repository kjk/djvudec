# djvudec — a plain-C DjVu decoder

This is a [DjVu](https://fileformats.dev/djvu) format decoder, like [libdjvu](https://github.com/barak/djvulibre), but better:
* smaller: `bun cmd/compare-code-sizes.ts -clang` shows a 293 KiB stripped probe executable (us) vs. 1.86 MiB for the same probe linked against libdjvu (clang -O3, Windows x64; `.text` 221 KiB vs. 1.21 MiB). With MSVC (`bun cmd/compare-code-sizes.ts`, -O2 -Ob3 -GL/-LTCG): 685 KiB vs. 2.41 MiB (`.text` 546 KiB vs. 1.84 MiB)
* plain C, no dependencies (vs. C++)
* simpler API
* simpler to integrate: copy dist/djvu.h and dist/djvu.c into your project. This is amalgamated src/* into a single file, like sqlite
* mostly faster, according to benchmarks (`bun cmd/bench.ts`)
* tested on thousands of .djvu files for bit-by-bit correctness to libdjvu

## API
See [`src/djvu.h`](src/djvu.h). Sketch:
```c
djvu_init();                                       /* once, before threads */
djvu_ctx *ctx = djvu_ctx_new(NULL, NULL, NULL, NULL, on_error, NULL);
djvu_doc *doc = djvu_doc_open(ctx, data, len);     /* data must outlive doc */
int n = djvu_doc_page_count(doc);
djvu_page_info info; djvu_doc_page_info(doc, 0, &info);
djvu_image *img = djvu_page_render(doc, 0, 1);     /* gray8 / rgb24 */
char *txt = djvu_page_text(doc, 0);
```

## Build & test
Requires `clang`, `bun`, and `git`. `cmd/get-deps.ts` clones the DjvuNet,
DjVuLibre, and DjvuNet/artifacts repos into `deps/`; the `.djvu` samples in
those checkouts are the test corpus. `build.ts` and `tests.ts` call it
automatically.
```
bun cmd/get-deps.ts     # clone deps (the corpus; auto-run below)
bun cmd/build.ts        # build reference tools + the C library and test harness
                        # (Windows: out/msvc/djvu_test_msvc.exe; -clang: out/clang/djvu_test_clang)
bun cmd/tests.ts -all   # build, then verify render + text against DjVuLibre
bun cmd/fuzz.ts -check-crashes  # replay tracked fuzz/crashes under ASan (CI)
```

CI (GitHub Actions): `.github/workflows/ci.yml` — full oracle tests + amalgamation
on Windows MSVC, Linux clang and macOS clang; fuzz crash regression (Windows ASan,
Linux ASan + UBSan); WASM decode smoke.

`djvu_test` CLI (jbig2dec-flavored). `-page N` is 1-based here, while the C API
is 0-based; `-out` writes PGM for gray pages and PPM for color:
```
djvu_test -info in.djvu
djvu_test -page N -out out.pnm in.djvu
djvu_test -page N -text in.djvu
```

## How it was made

This is an automatic, AI-assisted port of [DjvuNet](https://github.com/DjvuNet/DjvuNet) (C#)
DjVu decoder, done with Grok Build.

Correctness is verified against [DjVuLibre](https://github.com/DjvuNet/DjVuLibre)
(`ddjvu`, `djvutxt`) on the sample files in the `deps/` checkouts (DjVuLibre docs,
DjvuNet specs and test files, DjvuNet/artifacts), and on my own large collection of djvu files gathered for testing [SumatraPDF](https://www.sumatrapdfreader.org/).
