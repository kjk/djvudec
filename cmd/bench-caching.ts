// bench-caching.ts -- compare djvudec cache modes on the same file.
//
//   bun cmd/bench-caching.ts [file.djvu] [-clang] [-full] [-clean]
//
// Builds djvu_test, then runs `djvu_test -bench-caching` on the given file.
// Each mode (none / eager / on_demand) gets 2 session runs (open, render every
// page, close); then a best-of-2 comparison table (op | none | eager |
// on_demand | %none | %demand; % vs eager baseline).
// With no file, picks a random .djvu from testfiles/subset (`-full` →
// testfiles/full).
import { existsSync, readdirSync, statSync } from "fs";
import { join, dirname } from "path";
import { getDeps } from "./get-deps";
import { build, buildRef, cleanBuildOutput, defaultUseClang } from "./build";
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
    console.error(
      `no .djvu files under ${corpus} (run cmd/get-deps.ts or cmd/dump_features.ts --pick)`,
    );
    process.exit(1);
  }
  file = all[Math.floor(Math.random() * all.length)];
  console.log(`(random pick from ${corpus}) ${file}`);
} else if (!existsSync(file)) {
  console.error(`no such file: ${file}`);
  process.exit(1);
}

if (doClean) {
  console.log("clean: removing out/...");
  cleanBuildOutput();
}
await buildRef();
const TEST = await build(useClang);

const r = Bun.spawnSync({
  cmd: [TEST, "-bench-caching", file],
  stdout: "inherit",
  stderr: "inherit",
});
process.exit(r.exitCode ?? 0);