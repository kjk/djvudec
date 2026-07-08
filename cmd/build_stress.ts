// build_stress.ts -- build djvudec_stress (directory corpus stress harness).
//
//   bun cmd/build_stress.ts
//   bun cmd/build_stress.ts -clang
//   bun cmd/build_stress.ts -clean
import { cleanBuildOutput, defaultUseClang } from "./build";
import { buildLibTool, STRESS_TARGET } from "./build_lib";

export async function buildStress(useClang = defaultUseClang): Promise<string> {
  return buildLibTool(STRESS_TARGET, useClang);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  if (args.includes("-clean")) cleanBuildOutput();
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildStress(useClang);
}