// bench.ts -- benchmark our decoder against DjVuLibre ddjvuapi.
//
//   bun cmd/bench.ts <file.djvu ... | -rand N> [-clang] [-clean]
//
// Regenerates dist/ when src/ is newer (`-clean`: always regenerate dist/,
// delete out/, full rebuild). Builds djvu_test from dist/djvu.c
// (DjVuLibre via test/bench_ddjvu.cpp), then runs `djvu_test -bench` on each
// selected file. Session benchmark: open doc, render every page, close; 2
// runs each for djvudec and libdjvu. Prints one line per run (open, per-page,
// close ms), then a best-of-2 comparison table (op | libdjvu | djvudec |
// diff | %diff; + = djvudec slower).
// With no selection it prints usage + the available corpus file count.
import { getDeps } from "./get-deps";
import { buildDist } from "./build-dist";
import { buildRef, buildBench, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusSummary, selectFiles } from "./corpus";

const useClang = process.argv.includes("-clang") || defaultUseClang;
const doClean = process.argv.includes("-clean");

await getDeps();
const files = selectFiles(
  `usage: bun cmd/bench.ts <selection> [options]
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
  const r = Bun.spawnSync({ cmd: [TEST, "-bench", file], stdout: "inherit", stderr: "inherit" });
  if (r.exitCode) rc = r.exitCode;
}
process.exit(rc);
