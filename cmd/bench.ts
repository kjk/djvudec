// bench.ts -- benchmark our decoder against DjVuLibre ddjvuapi.
//
//   bun cmd/bench.ts [file.djvu] [-clang] [-full]
//
// Builds djvu_test (DjVuLibre via test/bench_ddjvu.cpp), then runs
// `djvu_test -bench` on the given file. Per page: 3 fresh doc opens (outside timer),
// fastest of 3 timed renders; no cross-render cache. Full render (decode + composite
// + rotation), 3 reps, fastest kept. At the end: whole-document session (open,
// render every page, extract text + annotations per page, close), 3 reps, fastest
// kept. With no file, picks a random .djvu from testfiles/subset (`-full` →
// testfiles/full). Lines print DjVuLibre ms, ours ms, delta (+ = slower).
import { existsSync, readdirSync, statSync } from "fs";
import { join, dirname } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build, defaultUseClang } from "./build";
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
const benchArgs = process.argv.slice(2).filter((a) => a !== "-clang" && a !== "-full");
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

await buildRef();
const TEST = await build(useClang);

const r = Bun.spawnSync({ cmd: [TEST, "-bench", file], stdout: "inherit", stderr: "inherit" });
process.exit(r.exitCode ?? 0);
