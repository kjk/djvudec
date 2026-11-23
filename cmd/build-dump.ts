// build-dump.ts -- build djvudec_dump (library-only inspector, no DjVuLibre).
//
//   bun cmd/build-dump.ts          MSVC on Windows, clang elsewhere
//   bun cmd/build-dump.ts -clang
//   bun cmd/build-dump.ts -clean   rebuild the dump objs + exe from scratch
//
// Links src/*.c + test/djvudec_dump.c only. Serves as an API usage example and
// a compilation smoke test for the decoder.
import { buildLibTool, DUMP_TARGET } from "./build-lib";
import { defaultUseClang } from "./util";

export async function buildDump(
  useClang = defaultUseClang,
  clean = false,
): Promise<string> {
  return buildLibTool(DUMP_TARGET, useClang, false, clean);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildDump(useClang, args.includes("-clean"));
}