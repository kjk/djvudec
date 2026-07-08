// build-stress.ts -- build djvudec_stress (directory corpus stress harness).
//
//   bun cmd/build-stress.ts
//   bun cmd/build-stress.ts -clang
//   bun cmd/build-stress.ts -asan
//   bun cmd/build-stress.ts -clean
import { cleanBuildOutput, defaultUseClang } from "./build";
import { buildLibTool, STRESS_TARGET } from "./build-lib";

export async function buildStress(
  useClang = defaultUseClang,
  asan = false,
): Promise<string> {
  return buildLibTool(STRESS_TARGET, useClang, asan);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  if (args.includes("-clean")) cleanBuildOutput();
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildStress(useClang, args.includes("-asan"));
}