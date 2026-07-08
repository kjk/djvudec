// build-thread.ts -- build djvudec_thread (concurrent API stress harness).
//
//   bun cmd/build-thread.ts          MSVC on Windows, clang elsewhere
//   bun cmd/build-thread.ts -clang
//   bun cmd/build-thread.ts -clean
import { cleanBuildOutput, defaultUseClang } from "./build";
import { buildLibTool, THREAD_TARGET } from "./build-lib";

export async function buildThread(useClang = defaultUseClang): Promise<string> {
  return buildLibTool(THREAD_TARGET, useClang);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  if (args.includes("-clean")) cleanBuildOutput();
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildThread(useClang);
}