// stress-test.ts -- multi-threaded render stress over .djvu files.
//
// Mimics SumatraPDF's per-page caching + concurrent render setup:
// opens each file once, renders every page from N worker threads, closes.
//
//   bun cmd/stress-test.ts <dir | file.djvu ... | -rand N>
//     -no-asan   plain release build (default: clang + ASan)
//     -clang     clang release (with -no-asan)
//     -clean     rebuild the stress exe from scratch
//     -cpu N     thread count (default: os.cpus().length - 2, at least 2)
//
// With no selection it prints usage + the available corpus file count.
import { cpus } from "node:os";
import { resolve } from "node:path";
import { defaultUseClang } from "./build";
import { buildStress } from "./build-stress";
import { getDeps } from "./get-deps";
import { corpusSummary, selectFiles } from "./corpus";

function defaultThreadCount(): number {
  return Math.max(2, cpus().length - 2);
}

const args = process.argv.slice(2);
const useClang = args.includes("-clang") || defaultUseClang;
const useAsan = !args.includes("-no-asan");
const clean = args.includes("-clean");
const cpuArg = args.indexOf("-cpu");
const cpuOverride = cpuArg >= 0 ? Number.parseInt(args[cpuArg + 1] ?? "", 10) : 0;
if (cpuArg >= 0 && (!Number.isFinite(cpuOverride) || cpuOverride < 1)) {
  console.error("-cpu requires a positive integer");
  process.exit(2);
}

await getDeps();
const paths = selectFiles(
  `usage: bun cmd/stress-test.ts <selection> [options]
selection (required; default prints this help):
  <dir>           stress every .djvu under the directory
  file.djvu ...   stress the given files
  -rand N         stress N randomly selected corpus files
options:
  -no-asan        plain release build (default: clang + ASan)
  -clang          clang release (with -no-asan)
  -clean          rebuild the stress exe from scratch
  -cpu N          thread count (default: cores - 2, at least 2)

${corpusSummary()}`,
  ["-rand", "-cpu"],
);

const ncpu = cpuOverride > 0 ? cpuOverride : defaultThreadCount();
const exe = await buildStress(useClang, useAsan, clean);
const proc = Bun.spawn([exe, "-cpu", String(ncpu), ...paths.map((p) => resolve(p))], {
  stdout: "inherit",
  stderr: "inherit",
});
process.exit(await proc.exited);