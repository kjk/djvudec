// bench-sum.ts -- benchmark replicating SumatraPDF's DjVu engines.
//
//   bun cmd/bench-sum.ts <file.djvu ... | -rand N> [-clang] [-clean]
//
// Same harness as cmd/bench.ts, but instead of timing the bare
// djvu_page_render(subsample=1) it replicates how SumatraPDF actually opens
// and renders pages:
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
//
// Regenerates dist/ when src/ is newer (`-clean`: full rebuild). Builds
// djvu_test from dist/djvu.c, then runs `djvu_test -bench-sum` on the file.
// Session benchmark (SumatraPDF render path): open doc, render every page,
// close; 2 runs each for djvudec and libdjvu. Prints one line per run, then
// best-of-2 comparison (open, each page, close; + = djvudec slower).
// With no selection it prints usage + the available corpus file count.
import { getDeps } from "./get-deps";
import { buildDist } from "./build-dist";
import { buildRef, buildBench, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusSummary, selectFiles } from "./corpus";

const useClang = process.argv.includes("-clang") || defaultUseClang;
const doClean = process.argv.includes("-clean");

await getDeps();
const files = selectFiles(
  `usage: bun cmd/bench-sum.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   bench the given files
  -rand N         bench N randomly selected corpus files
options:
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

let rc = 0;
for (const file of files) {
  if (files.length > 1) console.log(`\n=== ${file}`);
  const r = Bun.spawnSync({ cmd: [TEST, "-bench-sum", file], stdout: "inherit", stderr: "inherit" });
  if (r.exitCode) rc = r.exitCode;
}
process.exit(rc);
