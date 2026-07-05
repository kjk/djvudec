#!/usr/bin/env bun
// find_slower_pages.ts -- list pages where djvudec render is slower than libdjvu.
//
//   bun cmd/find_slower_pages.ts [-clang] [-full] [-clean] [-cpu N]
//
// Runs djvu_test -bench on every .djvu under the test corpus (default
// testfiles/djvu; -full → testfiles/full; DJVU_SPECS overrides). Writes
// document, page, and timings to slower.txt in the repo root.
import { existsSync, readdirSync, statSync, writeFileSync } from "fs";
import { cpus } from "os";
import { join, dirname, relative } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build, cleanBuildOutput, defaultUseClang } from "./build";

const ROOT = dirname(import.meta.dir);
const OUT_PATH = join(ROOT, "slower.txt");

export type BenchPageRow = {
  page: number;
  lib: number;
  ours: number;
  diff: number;
  pct: number;
};

export type SlowerPage = BenchPageRow & {
  file: string;
};

function walkDjvu(dir: string): string[] {
  if (!existsSync(dir)) return [];
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...walkDjvu(p));
    else if (name.toLowerCase().endsWith(".djvu")) out.push(p);
  }
  return out;
}

function corpusDir(): string {
  if (process.env.DJVU_SPECS) return process.env.DJVU_SPECS;
  const name = process.argv.includes("-full") ? "full" : "djvu";
  return join(ROOT, "testfiles", name);
}

export function parseBenchCompareTable(text: string): BenchPageRow[] {
  const rows: BenchPageRow[] = [];
  let inTable = false;

  for (const line of text.split(/\r?\n/)) {
    if (line.includes("(best of") && line.includes("djvudec slower")) {
      inTable = true;
      continue;
    }
    if (!inTable) continue;

    const m = line.match(
      /^(\d+)\s+([\d.]+)\s+([\d.]+)\s+([+\-][\d.]+)\s+([+\-][\d.]+%)\s*$/,
    );
    if (!m) continue;
    rows.push({
      page: parseInt(m[1]!, 10),
      lib: parseFloat(m[2]!),
      ours: parseFloat(m[3]!),
      diff: parseFloat(m[4]!),
      pct: parseFloat(m[5]!),
    });
  }
  return rows;
}

function fmtPct(pct: number): string {
  return `${pct >= 0 ? "+" : ""}${pct.toFixed(1)}%`;
}

function fmtDiff(diff: number): string {
  return `${diff >= 0 ? "+" : ""}${diff.toFixed(2)}`;
}

async function benchFile(testExe: string, file: string): Promise<string> {
  const proc = Bun.spawn({
    cmd: [testExe, "-bench", file],
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

function writeSlowerReport(entries: SlowerPage[]): void {
  const sorted = [...entries].sort((a, b) => b.diff - a.diff);
  const lines: string[] = [
    "# Pages where djvudec render is slower than libdjvu (best of 2 runs per page)",
    "# columns: document<TAB>page<TAB>libdjvu_ms<TAB>djvudec_ms<TAB>diff_ms<TAB>pct",
    "",
  ];

  for (const e of sorted) {
    lines.push(
      `${e.file}\t${e.page}\t${e.lib.toFixed(2)}\t${e.ours.toFixed(2)}\t` +
        `${fmtDiff(e.diff)}\t${fmtPct(e.pct)}`,
    );
  }

  const files = new Set(sorted.map((e) => e.file));
  lines.push("");
  lines.push(
    `# ${sorted.length} slower page(s) in ${files.size} file(s)`,
  );
  writeFileSync(OUT_PATH, `${lines.join("\n")}\n`);
}

async function main(): Promise<number> {
  const useClang = process.argv.includes("-clang") || defaultUseClang;
  const doClean = process.argv.includes("-clean");
  const cpuArg = process.argv.indexOf("-cpu");
  const ncpu =
    cpuArg >= 0 && cpuArg + 1 < process.argv.length
      ? Math.max(1, parseInt(process.argv[cpuArg + 1]!, 10) || 1)
      : cpus().length;

  if (doClean) cleanBuildOutput();

  await getDeps();
  await buildRef();
  const testExe = await build(useClang);

  const corpus = corpusDir();
  const files = walkDjvu(corpus).sort();
  if (files.length === 0) {
    console.error(`no .djvu files under ${corpus}`);
    return 1;
  }

  console.log(`corpus: ${corpus} (${files.length} files, ${ncpu} workers)`);
  console.log(`harness: ${testExe}`);

  const slower: SlowerPage[] = [];
  let done = 0;
  let failed = 0;
  let next = 0;

  async function worker(): Promise<void> {
    while (true) {
      const i = next++;
      if (i >= files.length) return;
      const file = files[i]!;
      const rel = relative(ROOT, file).replaceAll("\\", "/");
      try {
        const out = await benchFile(testExe, file);
        const pages = parseBenchCompareTable(out);
        let n = 0;
        for (const row of pages) {
          if (row.diff > 0) {
            slower.push({ file: rel, ...row });
            n++;
          }
        }
        done++;
        console.log(`[${done}/${files.length}] ${rel} — ${n} slower page(s)`);
      } catch (e) {
        failed++;
        done++;
        const msg = e instanceof Error ? e.message : String(e);
        console.error(`[${done}/${files.length}] ${rel} — FAILED: ${msg}`);
      }
    }
  }

  await Promise.all(Array.from({ length: Math.min(ncpu, files.length) }, () => worker()));

  writeSlowerReport(slower);
  console.log("");
  console.log(`wrote ${slower.length} slower page(s) to ${OUT_PATH}`);
  if (failed > 0) {
    console.error(`${failed} file(s) failed`);
    return 1;
  }
  return 0;
}

if (import.meta.main) {
  process.exit(await main());
}