// thread.ts -- run djvudec_thread concurrent stress harness.
//
//   bun cmd/thread.ts file.djvu
//   bun cmd/thread.ts -cpu 4 -nops 512 file.djvu
//   bun cmd/thread.ts -clang deps/DjVuLibre/doc/djvu3spec.djvu
//   bun cmd/thread.ts -tsan file.djvu   ThreadSanitizer build (Linux/macOS only)
import { defaultUseClang } from "./util";
import { buildThread } from "./build-thread";

const args = process.argv.slice(2);
const useClang = args.includes("-clang") || defaultUseClang;
const tsan = args.includes("-tsan");
const fwd = args.filter((a) => a !== "-clang" && a !== "-tsan");

if (fwd.length === 0 || fwd[0] === "-h" || fwd[0] === "--help") {
  console.error("usage: bun cmd/thread.ts [-clang] [-tsan] [-cpu N] [-nops N] file.djvu");
  process.exit(2);
}

const exe = await buildThread(useClang, false, tsan ? "tsan" : "none");
const proc = Bun.spawn([exe, ...fwd], { stdout: "inherit", stderr: "inherit" });
process.exit(await proc.exited);