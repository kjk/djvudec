// thread.ts -- run djvudec_thread concurrent stress harness.
//
//   bun cmd/thread.ts file.djvu
//   bun cmd/thread.ts -cpu 4 -nops 512 file.djvu
//   bun cmd/thread.ts -clang testfiles/subset/djvu3spec.djvu
import { $ } from "bun";
import { defaultUseClang } from "./build";
import { buildThread } from "./build_thread";

const args = process.argv.slice(2);
const useClang = args.includes("-clang") || defaultUseClang;
const fwd = args.filter((a) => a !== "-clang");

if (fwd.length === 0 || fwd[0] === "-h" || fwd[0] === "--help") {
  console.error("usage: bun cmd/thread.ts [-clang] [-cpu N] [-nops N] file.djvu");
  process.exit(2);
}

const exe = await buildThread(useClang);
const proc = Bun.spawn([exe, ...fwd], { stdout: "inherit", stderr: "inherit" });
process.exit(await proc.exited);