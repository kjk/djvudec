// bench.ts -- benchmark our decoder against DjVuLibre ddjvuapi.
//
//   bun cmd/bench.ts <file.djvu ... | -rand N | -all> [-like-sumatra] [-clang] [-clean]
//   bun cmd/bench.ts -list-files
//
// Regenerates dist/ when src/ is newer (`-clean`: always regenerate dist/,
// delete out/, full rebuild). Builds djvu_test from dist/djvu.c
// (DjVuLibre via test/bench_ddjvu.cpp), then runs `djvu_test -bench` on each
// selected file. Session benchmark: open doc, render every page, close; 2
// runs each for djvudec and libdjvu. Prints one line per run (open, per-page,
// close ms), then a best-of-2 comparison table (op | libdjvu | djvudec |
// diff | %diff; + = djvudec slower).
// With no selection it prints usage + the available corpus file count.
//
// -like-sumatra (formerly cmd/bench-sum.ts): same harness, but instead of
// timing the bare djvu_page_render(subsample=1) it replicates how SumatraPDF
// actually opens and renders pages (runs `djvu_test -bench-sum`):
//   - ours  -> EngineDjvuDec::RenderPage (src/EngineDjvuDec.cpp): pick an
//     integer subsample (compound pages forced to full res), decode with
//     djvu_ctx_set_bgr so color comes out BGR-ready (no per-pixel swap), pack
//     rows / copy gray8, rotate for subsample>1.
//   - libdjvu -> EngineDjVu::RenderPage (src/EngineDjVu.cpp): one
//     ddjvu_page_render into a BGR24 buffer at the mediabox size (page scaled
//     to fileDPI=300), letting ddjvu scale during decode.
// Both render at zoom=1, user-rotation=0. Per-page timings use a warm page:
// ours preloads Sjbz at doc-open; libdjvu runs ddjvu_page_decoding_done before
// the timer. The timed region is render-to-buffer only (GDI StretchBlt excluded).
import { readFileSync } from "fs";
import { dirname } from "path";
import { getDeps } from "./get-deps";
import { buildDist } from "./build-dist";
import { buildRef, buildBench, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusFiles, corpusSummary, fileLabel, selectFiles } from "./corpus";
import { parseDjvu } from "./djvu-parse";

const ROOT = dirname(import.meta.dir);
const useClang = process.argv.includes("-clang") || defaultUseClang;
const doClean = process.argv.includes("-clean");
const likeSumatra = process.argv.includes("-like-sumatra");

await getDeps();

// -list-files: relative path, size, page count of every corpus file.
if (process.argv.includes("-list-files")) {
  let pages = 0;
  const all = corpusFiles();
  for (const f of all) {
    let n = "?";
    try {
      const np = parseDjvu(new Uint8Array(readFileSync(f))).pages.length;
      pages += np;
      n = String(np);
    } catch {}
    console.log(`${fileLabel(f, ROOT)}, ${n} page(s)`);
  }
  console.log(`\n${all.length} file(s), ${pages} page(s)`);
  process.exit(0);
}

const files = selectFiles(
  `usage: bun cmd/bench.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   bench the given files
  -rand N         bench N randomly selected corpus files
  -all            bench every corpus file
  -list-files     list corpus files (path, size, pages) and exit
options:
  -like-sumatra   time the SumatraPDF engine render path (djvu_test -bench-sum)
                  instead of the bare full-res djvu_page_render
  -clang          build with clang instead of MSVC
  -clean          regenerate dist/ and delete out/ first

${corpusSummary()}`,
);

if (doClean) {
  console.log("clean: regenerating dist/...");
  await buildDist();
  console.log("clean: removing out/...");
  cleanBuildOutput();
}
await buildRef();
const TEST = await buildBench(useClang);

const benchFlag = likeSumatra ? "-bench-sum" : "-bench";
let rc = 0;
for (const file of files) {
  console.log(`${files.length > 1 ? "\n=== " : ""}${fileLabel(file, ROOT)}`);
  const r = Bun.spawnSync({ cmd: [TEST, benchFlag, file], stdout: "inherit", stderr: "inherit" });
  if (r.exitCode) rc = r.exitCode;
}
process.exit(rc);
