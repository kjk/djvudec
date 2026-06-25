// bench.ts -- benchmark our page decoder against DjVuLibre's.
//
//   bun cmd/bench.ts [file.djvu]
//
// Builds djvu_test (which links DjVuLibre via the bench shim), then runs
// `djvu_test -bench` on the given file. With no file, picks a random .djvu from
// testfiles/ (recursively). Per page it prints DjVuLibre time, our time, the
// absolute delta (+ = we're slower), and the percentage delta.
import { existsSync, readdirSync, statSync } from "fs";
import { join, dirname } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build } from "./build";

const ROOT = dirname(import.meta.dir);
const TEST = join(ROOT, "djvu_test.exe");

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

let file = process.argv[2];
if (!file) {
  // ensure the corpus exists, then pick a random file from testfiles/
  await getDeps();
  const all = walkDjvu(join(ROOT, "testfiles"));
  if (all.length === 0) {
    console.error("no .djvu files under testfiles/ (run cmd/get-deps.ts)");
    process.exit(1);
  }
  file = all[Math.floor(Math.random() * all.length)];
  console.log(`(random pick) ${file}`);
} else if (!existsSync(file)) {
  console.error(`no such file: ${file}`);
  process.exit(1);
}

await buildRef();
await build();

const r = Bun.spawnSync({ cmd: [TEST, "-bench", file], stdout: "inherit", stderr: "inherit" });
process.exit(r.exitCode ?? 0);
