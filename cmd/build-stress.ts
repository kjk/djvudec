// build-stress.ts -- build djvudec_stress (directory corpus stress harness).
//
//   bun cmd/build-stress.ts
//   bun cmd/build-stress.ts -clang
//   bun cmd/build-stress.ts -asan
//   bun cmd/build-stress.ts -tsan     ThreadSanitizer (Linux/macOS only)
//   bun cmd/build-stress.ts -clean    rebuild the stress objs + exe from scratch
import { buildLibTool, type Sanitizer, STRESS_TARGET } from "./build-lib";
import { defaultUseClang } from "./util";

export async function buildStress(
  useClang = defaultUseClang,
  san: Sanitizer | boolean = false,
  clean = false,
): Promise<string> {
  return buildLibTool(STRESS_TARGET, useClang, san, clean);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const useClang = args.includes("-clang") || defaultUseClang;
  const san = args.includes("-tsan") ? "tsan" : args.includes("-asan") ? "asan" : "none";
  await buildStress(useClang, san, args.includes("-clean"));
}