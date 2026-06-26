// bench-sum.ts -- benchmark replicating SumatraPDF's DjVu engines.
//
//   bun cmd/bench-sum.ts [file.djvu] [-clang] [-full] [-clean]
//
// Same harness as cmd/bench.ts, but instead of timing the bare
// djvu_page_render(subsample=1) it replicates how SumatraPDF actually opens
// and renders pages:
//   - ours  -> EngineDjvuDec::RenderPage (src/EngineDjvuDec.cpp): pick an
//     integer subsample (compound pages forced to full res), decode, convert
//     RGB->BGR (or copy gray8), rotate for subsample>1.
//   - libdjvu -> EngineDjVu::RenderPage (src/EngineDjVu.cpp): one
//     ddjvu_page_render into a BGR24 buffer at the mediabox size (page scaled
//     to fileDPI=300), letting ddjvu scale during decode.
// Both render at zoom=1, user-rotation=0. The timed region covers decode +
// pixel conversion only; the GDI StretchBlt/DIB step the engines do is
// excluded (it is not a decoder cost). This surfaces why libdjvu can win in
// SumatraPDF even though bench.ts shows our raw render is faster.
//
// Regenerates dist/ when src/ is newer (`-clean`: full rebuild). Builds
// djvu_test from dist/djvu.c, then runs `djvu_test -bench-sum` on the file.
// Per page: 3 fresh doc opens (outside timer), fastest of 3 timed renders.
// At the end: whole-document session (open, render every page, extract text +
// links per page, close), 3 reps, fastest kept. With no file, picks a random
// .djvu from testfiles/subset (`-full` -> testfiles/full). Lines print
// DjVuLibre ms, ours ms, delta (+ = ours slower).
import { existsSync, readdirSync, statSync } from "fs";
import { join, dirname } from "path";
import { getDeps } from "./get-deps";
import { buildDist } from "./build-dist";
import { buildRef, buildBench, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusDir } from "./corpus";

const ROOT = dirname(import.meta.dir);

function walkDjvu(dir: string): string[] {
  if (!existsSync(dir)) return [];
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...walkDjvu(p));
    else if (name.toLowerCase().endsWith(".djvu")) out.push(p);
  }
  return out;
}

const useClang = process.argv.includes("-clang") || defaultUseClang;
const doClean = process.argv.includes("-clean");
const benchArgs = process.argv.slice(2).filter(
  (a) => a !== "-clang" && a !== "-full" && a !== "-clean",
);
let file = benchArgs.find((a) => !a.startsWith("-"));
if (!file) {
  await getDeps();
  const corpus = corpusDir(ROOT);
  const all = walkDjvu(corpus);
  if (all.length === 0) {
    console.error(`no .djvu files under ${corpus} (run cmd/get-deps.ts or cmd/dump_features.ts --pick)`);
    process.exit(1);
  }
  file = all[Math.floor(Math.random() * all.length)];
  console.log(`(random pick from ${corpus}) ${file}`);
} else if (!existsSync(file)) {
  console.error(`no such file: ${file}`);
  process.exit(1);
}

if (doClean) {
  console.log("clean: regenerating dist/...");
  await buildDist();
  console.log("clean: removing out/...");
  cleanBuildOutput();
}
await buildRef();
const TEST = await buildBench(useClang);

const r = Bun.spawnSync({ cmd: [TEST, "-bench-sum", file], stdout: "inherit", stderr: "inherit" });
process.exit(r.exitCode ?? 0);
