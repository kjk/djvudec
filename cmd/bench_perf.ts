#!/usr/bin/env bun
// bench_perf.ts -- compare per-page render times (bench_before vs bench_after).
//
//   bun cmd/bench_perf.ts file.djvu              run both + compare
//   bun cmd/bench_perf.ts run before file.djvu   capture timings to stdout
//   bun cmd/bench_perf.ts run after file.djvu
//   bun cmd/bench_perf.ts compare before.txt after.txt
//
// Each timing line: pN t1 t2 t3 (ms, from -bench-render). Comparison uses the
// fastest of the three runs per page.
import { existsSync, readFileSync } from "fs";
import { defaultUseClang } from "./build";
import { benchTarget, libToolExePath } from "./build_lib";

export type PageTimings = Map<number, [number, number, number]>;

export function parseBenchOutput(text: string): PageTimings {
  const pages = new Map<number, [number, number, number]>();
  for (const line of text.split(/\r?\n/)) {
    const m = line.match(/^p(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$/);
    if (!m) continue;
    pages.set(parseInt(m[1], 10), [
      parseFloat(m[2]),
      parseFloat(m[3]),
      parseFloat(m[4]),
    ]);
  }
  return pages;
}

function min3(t: [number, number, number]): number {
  return Math.min(t[0], t[1], t[2]);
}

export function compareTimings(before: PageTimings, after: PageTimings): string {
  const pages = [...new Set([...before.keys(), ...after.keys()])].sort(
    (a, b) => a - b,
  );
  const lines: string[] = [];
  let sumB = 0;
  let sumA = 0;
  let n = 0;

  for (const p of pages) {
    const b = before.get(p);
    const a = after.get(p);
    if (!b || !a) {
      lines.push(`p${p} missing in ${!b ? "before" : "after"}`);
      continue;
    }
    const mb = min3(b);
    const ma = min3(a);
    const delta = ma - mb;
    const pct = mb > 0 ? (delta / mb) * 100 : 0;
    lines.push(
      `p${p} ${mb.toFixed(2)} => ${ma.toFixed(2)}, ${delta >= 0 ? "+" : ""}${delta.toFixed(2)} ms, ${pct >= 0 ? "+" : ""}${pct.toFixed(1)}%`,
    );
    sumB += mb;
    sumA += ma;
    n++;
  }

  if (n > 0) {
    const totalDelta = sumA - sumB;
    const totalPct = sumB > 0 ? (totalDelta / sumB) * 100 : 0;
    lines.push(
      `total (fastest/page) ${sumB.toFixed(2)} => ${sumA.toFixed(2)}, ` +
        `${totalDelta >= 0 ? "+" : ""}${totalDelta.toFixed(2)} ms, ` +
        `${totalPct >= 0 ? "+" : ""}${totalPct.toFixed(1)}% (${n} pages)`,
    );
  }
  return lines.join("\n");
}

async function runBenchRender(exe: string, file: string): Promise<string> {
  if (!existsSync(exe)) {
    throw new Error(`not found: ${exe} (run bun cmd/build_bench.ts … -clean)`);
  }
  const proc = Bun.spawn({
    cmd: [exe, "-bench-render", file],
    stdout: "pipe",
    stderr: "pipe",
  });
  const [out, err] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
  ]);
  const code = await proc.exited;
  if (code !== 0) {
    throw new Error(err.trim() || `exit ${code}`);
  }
  return out;
}

async function main(): Promise<number> {
  const args = process.argv.slice(2).filter((a) => a !== "-clang");
  const useClang = process.argv.includes("-clang") || defaultUseClang;

  if (args[0] === "compare") {
    const [beforePath, afterPath] = args.slice(1);
    if (!beforePath || !afterPath) {
      console.error("usage: bun cmd/bench_perf.ts compare before.txt after.txt");
      return 1;
    }
    const before = parseBenchOutput(readFileSync(beforePath, "utf8"));
    const after = parseBenchOutput(readFileSync(afterPath, "utf8"));
    console.log(compareTimings(before, after));
    return 0;
  }

  if (args[0] === "run") {
    const variant = args[1];
    const file = args[2];
    if ((variant !== "before" && variant !== "after") || !file) {
      console.error("usage: bun cmd/bench_perf.ts run before|after file.djvu");
      return 1;
    }
    if (!existsSync(file)) {
      console.error(`no such file: ${file}`);
      return 1;
    }
    const exe = libToolExePath(benchTarget(variant), useClang);
    const out = await runBenchRender(exe, file);
    process.stdout.write(out);
    return 0;
  }

  const file = args.find((a) => !a.startsWith("-"));
  if (!file) {
    console.error(
      "usage: bun cmd/bench_perf.ts file.djvu\n" +
        "       bun cmd/bench_perf.ts run before|after file.djvu\n" +
        "       bun cmd/bench_perf.ts compare before.txt after.txt",
    );
    return 1;
  }
  if (!existsSync(file)) {
    console.error(`no such file: ${file}`);
    return 1;
  }

  const beforeExe = libToolExePath(benchTarget("before"), useClang);
  const afterExe = libToolExePath(benchTarget("after"), useClang);
  console.log(`file: ${file}`);
  console.log(`before: ${beforeExe}`);
  const beforeOut = await runBenchRender(beforeExe, file);
  console.log(`after: ${afterExe}`);
  const afterOut = await runBenchRender(afterExe, file);

  const before = parseBenchOutput(beforeOut);
  const after = parseBenchOutput(afterOut);
  console.log("\n--- comparison (fastest of 3 runs per page; + = slower after) ---");
  console.log(compareTimings(before, after));
  return 0;
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});