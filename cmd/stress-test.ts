// stress-test.ts -- multi-threaded render stress over a .djvu directory.
//
// Mimics SumatraPDF's per-page caching + concurrent render setup:
// opens each file once, renders every page from N worker threads, closes.
//
//   bun cmd/stress-test.ts <dir>            clang + ASan build (default)
//   bun cmd/stress-test.ts -no-asan <dir>   plain release build
//   bun cmd/stress-test.ts -clang <dir>
//   bun cmd/stress-test.ts -clean <dir>     rebuild the stress exe from scratch
//   bun cmd/stress-test.ts -cpu 2 <dir>
//
// Default thread count: os.cpus().length - 2, at least 2.
import { cpus } from "node:os";
import { existsSync, statSync } from "node:fs";
import { resolve } from "node:path";
import { defaultUseClang } from "./build";
import { buildStress } from "./build-stress";

function defaultThreadCount(): number {
  return Math.max(2, cpus().length - 2);
}

function usage(): never {
  console.error("usage: bun cmd/stress-test.ts [-clang] [-no-asan] [-clean] [-cpu N] <dir>");
  process.exit(2);
}

const args = process.argv.slice(2);
const useClang = args.includes("-clang") || defaultUseClang;
const useAsan = !args.includes("-no-asan");
const clean = args.includes("-clean");
let cpuOverride = 0;
const fwd: string[] = [];

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "-clang" || a === "-no-asan" || a === "-clean") continue;
  if (a === "-cpu") {
    if (i + 1 >= args.length) usage();
    cpuOverride = Number.parseInt(args[++i], 10);
    if (!Number.isFinite(cpuOverride) || cpuOverride < 1) {
      console.error("-cpu requires a positive integer");
      process.exit(2);
    }
    continue;
  }
  if (a === "-h" || a === "--help") usage();
  if (a.startsWith("-")) usage();
  fwd.push(a);
}

if (fwd.length !== 1) usage();

const dir = resolve(fwd[0]);
if (!existsSync(dir) || !statSync(dir).isDirectory()) {
  console.error(`not a directory: ${dir}`);
  process.exit(1);
}

const ncpu = cpuOverride > 0 ? cpuOverride : defaultThreadCount();
const exe = await buildStress(useClang, useAsan, clean);
const proc = Bun.spawn([exe, "-cpu", String(ncpu), dir], {
  stdout: "inherit",
  stderr: "inherit",
});
process.exit(await proc.exited);