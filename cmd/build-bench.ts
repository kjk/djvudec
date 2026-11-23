// build-bench.ts -- build bench_before / bench_after (library-only render timers).
//
//   bun cmd/build-bench.ts before [-clean] [-clang]
//   bun cmd/build-bench.ts after  [-clean] [-clang]
//
// Each variant links src/*.c + test/djvudec_dump.c into its own out/bench_* tree
// so you can snapshot a binary before edits and rebuild after without overwriting.
//
// `before` is the baseline SNAPSHOT: run it before editing src/. This script is
// the only thing that (re)builds it -- bench-perf.ts and profile-render.ts
// require it to exist and never rebuild it (they refresh only `after`).
import { rmSync } from "fs";
import { benchTarget, buildLibTool } from "./build-lib";
import { defaultUseClang, ROOT } from "./util";

function cleanVariant(variant: "before" | "after") {
  rmSync(`${ROOT}/out/bench_${variant}`, { recursive: true, force: true });
}

if (import.meta.main) {
  const args = process.argv.slice(2).filter((a) => a !== "-clang");
  const useClang = process.argv.includes("-clang") || defaultUseClang;
  const variant = args.find((a) => a === "before" || a === "after") as
    | "before"
    | "after"
    | undefined;
  if (!variant) {
    console.error("usage: bun cmd/build-bench.ts before|after [-clean] [-clang]");
    process.exit(1);
  }
  if (args.includes("-clean")) cleanVariant(variant);
  await buildLibTool(benchTarget(variant), useClang);
}