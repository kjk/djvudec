# Curated test corpus (`testfiles/subset`)

Twelve `.djvu` files copied from `testfiles/full` by `cmd/dump_features.ts --pick`.
Together they exercise every feature tag observed across the 726-file full corpus
(20 unique tags). `bgjp` and `fgjp` do not appear anywhere in `full`.

**Stats:** 12 files, 246 pages, ~6.1 s total render (measured by `-dump-features`).

`bun cmd/tests.ts` and `bun cmd/bench.ts` use this directory by default.
Pass `-full` to scan `testfiles/full` instead, or set `DJVU_SPECS` to any path.

## Regenerating the subset

```bash
bun cmd/dump_features.ts testfiles/full    # scan → features.jsonl
bun cmd/dump_features.ts --pick-only        # re-pick + copy to testfiles/subset
```

## Feature coverage

| Feature | File(s) |
| --- | --- |
| `container:single` | `1737 - invalid DPI value.djvu` |
| `container:indirect` (0-page bundle) | `directory.djvu` |
| `container:bundled` | most files |
| Shared dictionary (`djvi`, `incl`, `incl_djbz`) | `1998_zcoder`, `1998_distribution`, `djvu2spec`, `djvu3spec`, Barron |
| Inline Djbz (`inline_djbz`) | `bug-3125-rotated-pages.djvu` |
| Outline (`outline`) | `djvu2spec`, `djvu3spec`, `test0` |
| Hidden text (`text`) | most files |
| Annotations (`annot`) | `test0.djvu` |
| Hyperlinks (`links`) | `test0.djvu` |
| Bitonal pages (`type:bitonal`, `kind:mask`) | codec fixtures, specs, `test0` |
| Photo-only page (`type:photo`, `kind:bg`) | `mtorrent_anons_screen.djvu` |
| Compound pages (`type:compound`, `kind:bg`) | `1998_compression`, `1998_lossy_masked`, `djvu3spec`, `test0`, Barron |
| Blank / other-kind page (`kind:other`) | Barron (1 of 71 pages) |
| FGbz foreground palette (`fgbz`) | `djvu3spec`, `test0` |
| FG44 foreground (`fg44`) | `1998_compression`, `1998_lossy_masked`, Barron |
| Rotation (`rotation`) | `bug-3125-rotated-pages.djvu` |
| Z-coder regression | `1998_zcoder.djvu` |
| Compression regression | `1998_compression.djvu` |
| Lossy masked IW44 | `1998_lossy_masked.djvu` |
| Shared-dict distribution | `1998_distribution.djvu` |
| Spec conformance | `djvu2spec.djvu`, `djvu3spec.djvu` |

## Per-file summary

| File | Pages | Render | Tags / notes |
| --- | ---: | ---: | --- |
| `1737 - invalid DPI value.djvu` | 1 | 0.1 ms | Single-page FORM; invalid INFO DPI edge case |
| `1998_compression.djvu` | 25 | 2058 ms | Bundled compound; FG44; codec stress (see known failure below) |
| `1998_distribution.djvu` | 4 | 52 ms | INCL → shared Djbz via DJVI |
| `1998_lossy_masked.djvu` | 10 | 743 ms | Lossy masked IW44 composite |
| `1998_zcoder.djvu` | 10 | 69 ms | Z-coder table edge cases |
| `Barron D. Recursive Techniques…_CsAl_.djvu` | 71 | 678 ms | Smallest file with `kind:other`; also compound + FG44 |
| `bug-3125-rotated-pages.djvu` | 8 | 531 ms | Inline Djbz; 90°/180°/270° rotation |
| `directory.djvu` | 0 | 0 ms | Indirect DJVM shell (no embedded pages) |
| `djvu2spec.djvu` | 39 | 338 ms | DjVu spec v2; outline + shared dict |
| `djvu3spec.djvu` | 71 | 1412 ms | DjVu spec v3; FGbz + compound |
| `mtorrent_anons_screen.djvu` | 1 | 12 ms | Pure photo (BG44-only, no mask) |
| `test0.djvu` | 6 | 171 ms | Outline, annotations, hyperlinks, FGbz |

## Known expected failure

`1998_compression.djvu` page 19 reports a render mismatch vs ddjvu. This is a
documented ddjvu three-layer-stencil quirk (a few FG pixels ~1 px off from the
JB2 mask), not a decode bug. Layer internals match DjVuLibre byte-for-byte.

## Running tests

```bash
bun cmd/tests.ts              # subset (default)
bun cmd/tests.ts -full        # all of testfiles/full
DJVU_SPECS=/path bun cmd/tests.ts   # custom directory
```

```bash
bun cmd/bench.ts              # random file from subset
bun cmd/bench.ts -full        # random file from full corpus
bun cmd/bench.ts path/to/file.djvu
```