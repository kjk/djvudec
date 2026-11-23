#!/usr/bin/env bun
// bench-perf.ts -- compare per-page render times (bench_before vs bench_after).
//
//   bun cmd/bench-perf.ts file.djvu              run both + compare
//   bun cmd/bench-perf.ts -p 3 file.djvu        single page only (-page / --page too)
//   bun cmd/bench-perf.ts -warm 1 -layers file.djvu
//   bun cmd/bench-perf.ts run before file.djvu   capture timings to stdout
//   bun cmd/bench-perf.ts compare before.txt after.txt
//
// bench_after is built / refreshed from the current sources automatically.
// bench_before is a snapshot: it must already exist (`bun cmd/build-bench.ts
// before`, run BEFORE editing src/) and is never rebuilt here -- an implicit
// rebuild would recompile it from the edited sources and compare after with
// after. Each timing line: pN t1 t2 (ms, from -bench-render). Comparison uses
// the fastest of the two runs per page. With -layers, also:
//   layer pN jb2 t1 t2 iw44 t1 t2 composite t1 t2 rotate t1 t2
import { existsSync, readFileSync } from "fs";
import { benchExe } from "./build-lib";
import { getDeps } from "./get-deps";
import { corpusSummary, selectFiles } from "./corpus";
import { defaultUseClang } from "./util";

export type PageTimings = Map<number, [number, number]>;

export type LayerName = "jb2" | "iw44" | "composite" | "rotate";

export type PageLayerTimings = Map<
  number,
  Record<LayerName, [number, number]>
>;

export type BenchOpts = {
  warm: number;
  layers: boolean;
  page: number | null;
};

const LAYER_NAMES: LayerName[] = ["jb2", "iw44", "composite", "rotate"];

export function parseBenchOutput(text: string): PageTimings {
  const pages = new Map<number, [number, number]>();
  for (const line of text.split(/\r?\n/)) {
    const m = line.match(/^p(\d+)\s+([\d.]+)\s+([\d.]+)\s*$/);
    if (!m) continue;
    pages.set(parseInt(m[1], 10), [
      parseFloat(m[2]),
      parseFloat(m[3]),
    ]);
  }
  return pages;
}

export function parseLayerOutput(text: string): PageLayerTimings {
  const pages = new Map<
    number,
    Record<LayerName, [number, number]>
  >();
  for (const line of text.split(/\r?\n/)) {
    const m = line.match(
      /^layer\s+p(\d+)\s+jb2\s+([\d.]+)\s+([\d.]+)\s+iw44\s+([\d.]+)\s+([\d.]+)\s+composite\s+([\d.]+)\s+([\d.]+)\s+rotate\s+([\d.]+)\s+([\d.]+)\s*$/,
    );
    if (!m) continue;
    pages.set(parseInt(m[1], 10), {
      jb2: [parseFloat(m[2]), parseFloat(m[3])],
      iw44: [parseFloat(m[4]), parseFloat(m[5])],
      composite: [parseFloat(m[6]), parseFloat(m[7])],
      rotate: [parseFloat(m[8]), parseFloat(m[9])],
    });
  }
  return pages;
}

function min2(t: [number, number]): number {
  return Math.min(t[0], t[1]);
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
    const mb = min2(b);
    const ma = min2(a);
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

export function compareLayerTimings(
  before: PageLayerTimings,
  after: PageLayerTimings,
): string {
  const pages = [...new Set([...before.keys(), ...after.keys()])].sort(
    (a, b) => a - b,
  );
  const lines: string[] = [];
  const totals: Record<LayerName, { b: number; a: number; n: number }> = {
    jb2: { b: 0, a: 0, n: 0 },
    iw44: { b: 0, a: 0, n: 0 },
    composite: { b: 0, a: 0, n: 0 },
    rotate: { b: 0, a: 0, n: 0 },
  };

  for (const p of pages) {
    const b = before.get(p);
    const a = after.get(p);
    if (!b || !a) {
      lines.push(`p${p} layers missing in ${!b ? "before" : "after"}`);
      continue;
    }
    const parts: string[] = [];
    for (const layer of LAYER_NAMES) {
      const mb = min2(b[layer]);
      const ma = min2(a[layer]);
      const delta = ma - mb;
      const pct = mb > 0 ? (delta / mb) * 100 : 0;
      parts.push(
        `${layer} ${mb.toFixed(2)} => ${ma.toFixed(2)}, ${delta >= 0 ? "+" : ""}${delta.toFixed(2)} ms, ${pct >= 0 ? "+" : ""}${pct.toFixed(1)}%`,
      );
      totals[layer].b += mb;
      totals[layer].a += ma;
      totals[layer].n++;
    }
    lines.push(`p${p} ${parts.join("; ")}`);
  }

  for (const layer of LAYER_NAMES) {
    const t = totals[layer];
    if (t.n === 0) continue;
    const delta = t.a - t.b;
    const pct = t.b > 0 ? (delta / t.b) * 100 : 0;
    lines.push(
      `total ${layer} ${t.b.toFixed(2)} => ${t.a.toFixed(2)}, ` +
        `${delta >= 0 ? "+" : ""}${delta.toFixed(2)} ms, ` +
        `${pct >= 0 ? "+" : ""}${pct.toFixed(1)}% (${t.n} pages)`,
    );
  }
  return lines.join("\n");
}

// Every spelling of the page flag; all take a value (see VALUE_FLAGS).
const PAGE_FLAGS = ["-p", "-page", "--page"];

// Flags whose next argument is a value, not a file path.
const VALUE_FLAGS = ["-rand", "-warm", ...PAGE_FLAGS];

function isPageFlag(arg: string): boolean {
  return PAGE_FLAGS.includes(arg);
}

export function parseBenchOpts(argv: string[]): BenchOpts {
  const opts: BenchOpts = { warm: 0, layers: false, page: null };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "-warm" && i + 1 < argv.length) {
      opts.warm = parseInt(argv[++i], 10);
      if (Number.isNaN(opts.warm) || opts.warm < 0) opts.warm = 0;
    } else if (argv[i] === "-layers") {
      opts.layers = true;
    } else if (isPageFlag(argv[i]) && i + 1 < argv.length) {
      const page = parseInt(argv[++i], 10);
      if (!Number.isNaN(page) && page > 0) opts.page = page;
    }
  }
  return opts;
}

function filterBenchArgs(argv: string[]): string[] {
  const out: string[] = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "-clang" || a === "-layers") continue;
    if (a === "-warm" || isPageFlag(a)) {
      i++;
      continue;
    }
    out.push(a);
  }
  return out;
}

async function runBenchRender(
  exe: string,
  file: string,
  opts: BenchOpts,
): Promise<string> {
  const cmd = [exe, "-bench-render"];
  if (opts.warm > 0) cmd.push("-warm", String(opts.warm));
  if (opts.layers) cmd.push("-layers");
  if (opts.page != null) cmd.push("-page", String(opts.page));
  cmd.push(file);
  const proc = Bun.spawn({
    cmd,
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
  const rawArgs = process.argv.slice(2);
  const useClang = rawArgs.includes("-clang") || defaultUseClang;
  const benchOpts = parseBenchOpts(rawArgs);
  const args = filterBenchArgs(rawArgs);

  if (args[0] === "compare") {
    const [beforePath, afterPath] = args.slice(1);
    if (!beforePath || !afterPath) {
      console.error("usage: bun cmd/bench-perf.ts compare before.txt after.txt");
      return 1;
    }
    const beforeText = readFileSync(beforePath, "utf8");
    const afterText = readFileSync(afterPath, "utf8");
    console.log(compareTimings(parseBenchOutput(beforeText), parseBenchOutput(afterText)));
    const beforeLayers = parseLayerOutput(beforeText);
    const afterLayers = parseLayerOutput(afterText);
    if (beforeLayers.size > 0 || afterLayers.size > 0) {
      console.log(
        "\n--- layer breakdown (fastest of 2 runs per stage; + = slower after) ---",
      );
      console.log(compareLayerTimings(beforeLayers, afterLayers));
    }
    return 0;
  }

  if (args[0] === "run") {
    const variant = args[1];
    const file = args[2];
    if ((variant !== "before" && variant !== "after") || !file) {
      console.error(
        "usage: bun cmd/bench-perf.ts run before|after file.djvu [-warm N] [-layers] [-p N]",
      );
      return 1;
    }
    if (!existsSync(file)) {
      console.error(`no such file: ${file}`);
      return 1;
    }
    // Build chatter goes to stdout too; send it to stderr so `> before.txt`
    // captures only the timing lines.
    const log = console.log;
    console.log = console.error;
    const exe = await benchExe(variant, useClang);
    console.log = log;
    const out = await runBenchRender(exe, file, benchOpts);
    process.stdout.write(out);
    return 0;
  }

  await getDeps();
  const files = selectFiles(
    `usage: bun cmd/bench-perf.ts <file.djvu ... | -rand N | -all> [-warm N] [-layers] [-p N] [-clang]
       bun cmd/bench-perf.ts run before|after file.djvu [-warm N] [-layers] [-p N]
       bun cmd/bench-perf.ts compare before.txt after.txt

Needs the "before" snapshot: run \`bun cmd/build-bench.ts before\` on the
baseline sources first. bench_after is rebuilt from the current sources.

${corpusSummary()}`,
    VALUE_FLAGS,
  );

  // before: the existing snapshot (never rebuilt); after: current sources.
  const beforeExe = await benchExe("before", useClang);
  console.log(`before: ${beforeExe} (snapshot, not rebuilt)`);
  const afterExe = await benchExe("after", useClang);
  console.log(`after:  ${afterExe}`);

  if (benchOpts.page != null) console.log(`page: ${benchOpts.page}`);
  if (benchOpts.warm > 0) console.log(`warm: ${benchOpts.warm} renders/page`);
  if (benchOpts.layers) console.log("layers: jb2 / iw44 / composite / rotate");

  for (const file of files) {
    console.log(`file: ${file}`);
    const beforeOut = await runBenchRender(beforeExe, file, benchOpts);
    const afterOut = await runBenchRender(afterExe, file, benchOpts);

    const before = parseBenchOutput(beforeOut);
    const after = parseBenchOutput(afterOut);
    console.log("\n--- comparison (fastest of 2 runs per page; + = slower after) ---");
    console.log(compareTimings(before, after));

    if (benchOpts.layers) {
      const beforeLayers = parseLayerOutput(beforeOut);
      const afterLayers = parseLayerOutput(afterOut);
      console.log(
        "\n--- layer breakdown (fastest of 2 runs per stage; + = slower after) ---",
      );
      console.log(compareLayerTimings(beforeLayers, afterLayers));
    }
  }
  return 0;
}

main().then(
  (code) => process.exit(code),
  (e) => {
    console.error(e instanceof Error ? e.message : e);
    process.exit(1);
  },
);