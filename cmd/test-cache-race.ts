// test-cache-race.ts -- regression test for concurrent render vs page-cache drop.
//
//   bun cmd/test-cache-race.ts <file.djvu ... | -rand N | -all> [options]
//     -secs N    seconds to run per file (default 5)
//     -sub N     render subsample (default: the harness's own default)
//     -cpu N     render threads (default: the harness's own default)
//     -no-asan   plain release build (default: clang + AddressSanitizer)
//     -tsan      clang + ThreadSanitizer instead (Linux/macOS only)
//     -clang     clang release (with -no-asan; MSVC is the Windows default)
//     -clean     rebuild the test exe from scratch
//
// Builds test/crash_cache_race.c against the decoder (out/clang_asan/
// crash_cache_race_asan[.exe] by default) and runs it on each selected file:
// N threads render random pages through the per-page cache while another
// thread keeps calling djvu_doc_drop_page_cache, the way SumatraPDF's engine
// does under memory pressure. A use-after-free shows up as an ASan report (or
// an access violation without ASan); either way the exe exits non-zero and so
// does this script. With no selection it prints usage + the corpus file count.
import { resolve } from "node:path";
import { buildLibTool, CACHE_RACE_TARGET } from "./build-lib";
import { getDeps } from "./get-deps";
import { corpusSummary, selectFiles } from "./corpus";
import { defaultUseClang } from "./util";

const VALUE_FLAGS = ["-rand", "-secs", "-sub", "-cpu"];

const args = process.argv.slice(2);
const useClang = args.includes("-clang") || defaultUseClang;
const useTsan = args.includes("-tsan");
const useAsan = !useTsan && !args.includes("-no-asan");
const clean = args.includes("-clean");

// Positive integer value of a flag, or null when the flag is absent.
function intFlag(name: string): number | null {
  const i = args.indexOf(name);
  if (i < 0) return null;
  const n = Number.parseInt(args[i + 1] ?? "", 10);
  if (!Number.isFinite(n) || n < 1) {
    console.error(`${name} requires a positive integer`);
    process.exit(2);
  }
  return n;
}
const secs = intFlag("-secs") ?? 5;
const sub = intFlag("-sub");
const cpu = intFlag("-cpu");

await getDeps();
const files = selectFiles(
  `usage: bun cmd/test-cache-race.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   test the given files
  -rand N         test N randomly selected corpus files
  -all            test every corpus file
options:
  -secs N         seconds to run per file (default 5)
  -sub N          render subsample (default: harness default)
  -cpu N          render threads (default: harness default)
  -no-asan        plain release build (default: clang + ASan)
  -tsan           clang + ThreadSanitizer instead (Linux/macOS only)
  -clang          clang release (with -no-asan)
  -clean          rebuild the test exe from scratch

${corpusSummary()}`,
  VALUE_FLAGS,
);

const exe = await buildLibTool(
  CACHE_RACE_TARGET,
  useClang,
  useTsan ? "tsan" : useAsan ? "asan" : "none",
  clean,
);
const fwd = ["-secs", String(secs)];
if (sub != null) fwd.push("-sub", String(sub));
if (cpu != null) fwd.push("-cpu", String(cpu));

const failed: string[] = [];
for (const f of files) {
  const proc = Bun.spawn([exe, ...fwd, resolve(f)], {
    stdout: "inherit",
    stderr: "inherit",
  });
  const code = await proc.exited;
  if (code !== 0) {
    console.error(`[fail] ${f} (exit ${code})`);
    failed.push(f);
  } else {
    console.log(`[ok] ${f}`);
  }
}
console.log(
  `test-cache-race: ${files.length - failed.length}/${files.length} file(s) ok` +
    (failed.length ? `; FAILED: ${failed.join(", ")}` : ""),
);
process.exit(failed.length ? 1 : 0);
