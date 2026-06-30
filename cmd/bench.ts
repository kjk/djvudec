// bench.ts -- benchmark our decoder against DjVuLibre ddjvuapi.
//
//   bun cmd/bench.ts [file.djvu] [-clang] [-full] [-clean] [-eager]
//
// Regenerates dist/ when src/ is newer (`-clean`: always regenerate dist/,
// delete out/, full rebuild). Builds djvu_test from dist/djvu.c
// (DjVuLibre via test/bench_ddjvu.cpp), then runs `djvu_test -bench` on the
// given file. djvudec uses on-demand caching by default (`-eager` for preload-at-
// open). Session benchmark: open doc, render every page, close; 2 runs each for
// djvudec and libdjvu. Prints one line per run (open, per-page, close ms),
// then a best-of-2 comparison table (op | libdjvu | djvudec | diff | %diff;
// + = djvudec slower).
// With no file, picks a random .djvu from testfiles/subset (`-full` →
// testfiles/full).
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
const benchEager = process.argv.includes("-eager");
const benchArgs = process.argv.slice(2).filter(
  (a) => a !== "-clang" && a !== "-full" && a !== "-clean" && a !== "-eager",
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

const testArgs = [TEST, "-bench", ...(benchEager ? ["-eager"] : []), file];
const r = Bun.spawnSync({ cmd: testArgs, stdout: "inherit", stderr: "inherit" });
process.exit(r.exitCode ?? 0);
