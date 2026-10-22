// test-oversize-iw44.ts -- regression for the IW44 bw*bh / nb int overflow.
//
//   bun cmd/test-oversize-iw44.ts            build + run (msvc)
//   bun cmd/test-oversize-iw44.ts -asan      build + run under ASan
//   bun cmd/test-oversize-iw44.ts -clean     rebuild from scratch
//
// Builds test/oversize_iw44.c against the decoder and runs it. The test renders
// a crafted 65535x65535 BG44 page under a budgeted allocator: the fix rejects
// the oversize dimensions in map_new so no multi-GB buffer is ever requested
// (exit 0); pre-fix the render path asks for gigabytes (exit 1).
import { $ } from "bun";
import { defaultUseClang } from "./build";
import { buildLibTool, OVERSIZE_IW44_TARGET } from "./build-lib";

export async function testOversizeIw44(
  useClang = defaultUseClang,
  asan = false,
  clean = false,
): Promise<void> {
  const exe = await buildLibTool(OVERSIZE_IW44_TARGET, useClang, asan, clean);
  await $`${exe}`;
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const useClang = args.includes("-clang") || defaultUseClang;
  await testOversizeIw44(useClang, args.includes("-asan"), args.includes("-clean"));
}
