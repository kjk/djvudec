// build-thread.ts -- build djvudec_thread (concurrent API stress harness).
//
//   bun cmd/build-thread.ts          MSVC on Windows, clang elsewhere
//   bun cmd/build-thread.ts -clang
//   bun cmd/build-thread.ts -clean   rebuild the thread objs + exe from scratch
import { buildLibTool, THREAD_TARGET } from "./build-lib";
import { defaultUseClang } from "./util";

export async function buildThread(
  useClang = defaultUseClang,
  clean = false,
): Promise<string> {
  return buildLibTool(THREAD_TARGET, useClang, false, clean);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildThread(useClang, args.includes("-clean"));
}