// build_dump.ts -- build djvudec_dump (library-only inspector, no DjVuLibre).
//
//   bun cmd/build_dump.ts          MSVC on Windows, clang elsewhere
//   bun cmd/build_dump.ts -clang
//   bun cmd/build_dump.ts -clean   delete out/ first
//
// Links src/*.c + test/djvudec_dump.c only. Serves as an API usage example and
// a compilation smoke test for the decoder.
import { cleanBuildOutput, defaultUseClang } from "./build";
import { buildLibTool, DUMP_TARGET } from "./build_lib";

export async function buildDump(useClang = defaultUseClang): Promise<string> {
  return buildLibTool(DUMP_TARGET, useClang);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  if (args.includes("-clean")) cleanBuildOutput();
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildDump(useClang);
}