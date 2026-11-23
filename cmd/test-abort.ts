// test-abort.ts -- run the abort-API test (`djvu_test -verify-abort`) per file.
//
//   bun cmd/test-abort.ts <file.djvu ... | -rand N | -all> [-clang]
//
// Builds the djvu_test harness with build() (cmd/build.ts; MSVC by default on
// Windows, -clang for clang) and runs `djvu_test -verify-abort <file>` on each
// selected file. The harness exits 0 when the abort API behaves, non-zero
// otherwise; this script exits 1 if any file fails. With no selection it
// prints usage + the corpus file count. Independent of cmd/tests.ts.
import { resolve } from "node:path";
import { build } from "./build";
import { getDeps } from "./get-deps";
import { corpusSummary, selectFiles } from "./corpus";
import { defaultUseClang } from "./util";

const useClang = process.argv.includes("-clang") || defaultUseClang;

await getDeps();
const files = selectFiles(
  `usage: bun cmd/test-abort.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   test the given files
  -rand N         test N randomly selected corpus files
  -all            test every corpus file
options:
  -clang          build the clang harness instead of MSVC

${corpusSummary()}`,
);

const exe = await build(useClang);

const failed: string[] = [];
for (const f of files) {
  const proc = Bun.spawn([exe, "-verify-abort", resolve(f)], {
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
  `test-abort: ${files.length - failed.length}/${files.length} file(s) ok` +
    (failed.length ? `; FAILED: ${failed.join(", ")}` : ""),
);
process.exit(failed.length ? 1 : 0);
