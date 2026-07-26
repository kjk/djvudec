// bench.ts -- benchmark our decoder against DjVuLibre ddjvuapi.
//
//   bun cmd/bench.ts <file.djvu ... | -rand N | -all> [-like-sumatra] [-verbose]
//                     [-clang] [-clean]
//   bun cmd/bench.ts -list-files
//
// Regenerates dist/ when src/ is newer (`-clean`: always regenerate dist/,
// delete out/, full rebuild). Builds djvu_test from dist/djvu.c
// (DjVuLibre via test/bench_ddjvu.cpp), then runs `djvu_test -bench` on each
// selected file. Session benchmark: open doc, render every page, close; 2
// runs each for djvudec and libdjvu. Best-of-2 comparison (op | libdjvu |
// djvudec | diff | %diff; + = djvudec slower).
//
// Default output per file: table header + total row only. After all files:
// wall-clock elapsed and the top 10 pages where djvudec is slowest vs libdjvu
// (by absolute ms diff). `-verbose` prints the full per-file table (open,
// every page, close, total) plus the document alloc line.
// With no selection it prints usage + the available corpus file count.
//
// -like-sumatra (formerly cmd/bench-sum.ts): same harness, but instead of
// timing the bare djvu_page_render(subsample=1) it replicates how SumatraPDF
// actually opens and renders pages (runs `djvu_test -bench-sum`):
//   - ours  -> EngineDjvuDec::RenderPage (src/EngineDjvuDec.cpp): pick an
//     integer subsample (compound pages forced to full res), decode with
//     djvu_ctx_set_bgr so color comes out BGR-ready (no per-pixel swap), pack
//     rows / copy gray8, rotate for subsample>1.
//   - libdjvu -> EngineDjVu::RenderPage (src/EngineDjVu.cpp): one
//     ddjvu_page_render into a BGR24 buffer at the mediabox size (page scaled
//     to fileDPI=300), letting ddjvu scale during decode.
// Both render at zoom=1, user-rotation=0. Per-page timings use a warm page:
// ours preloads Sjbz at doc-open; libdjvu runs ddjvu_page_decoding_done before
// the timer. The timed region is render-to-buffer only (GDI StretchBlt excluded).
import { readFileSync } from "fs";
import { dirname } from "path";
import { getDeps } from "./get-deps";
import { buildDist } from "./build-dist";
import { buildRef, buildBench, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusFiles, corpusSummary, fileLabel, selectFiles } from "./corpus";
import { parseDjvu } from "./djvu-parse";

const ROOT = dirname(import.meta.dir);
const useClang = process.argv.includes("-clang") || defaultUseClang;
const doClean = process.argv.includes("-clean");
const likeSumatra = process.argv.includes("-like-sumatra");
const verbose = process.argv.includes("-verbose");

await getDeps();

// -list-files: relative path, size, page count of every corpus file.
if (process.argv.includes("-list-files")) {
  let pages = 0;
  const all = corpusFiles();
  for (const f of all) {
    let n = "?";
    try {
      const np = parseDjvu(new Uint8Array(readFileSync(f))).pages.length;
      pages += np;
      n = String(np);
    } catch {}
    console.log(`${fileLabel(f, ROOT)}, ${n} page(s)`);
  }
  console.log(`\n${all.length} file(s), ${pages} page(s)`);
  process.exit(0);
}

const files = selectFiles(
  `usage: bun cmd/bench.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   bench the given files
  -rand N         bench N randomly selected corpus files
  -all            bench every corpus file
  -list-files     list corpus files (path, size, pages) and exit
options:
  -like-sumatra   time the SumatraPDF engine render path (djvu_test -bench-sum)
                  instead of the bare full-res djvu_page_render
  -verbose        print full per-file table (open/pages/close/total + allocs)
  -clang          build with clang instead of MSVC
  -clean          regenerate dist/ and delete out/ first

Default: header + total row per file, then elapsed and top 10 slowest pages
vs libdjvu (by ms diff; + = djvudec slower).

${corpusSummary()}`,
);

if (doClean) {
  console.log("clean: regenerating dist/...");
  await buildDist();
  console.log("clean: removing out/...");
  cleanBuildOutput();
}
await buildRef();
const TEST = await buildBench(useClang);

function formatElapsed(ms: number): string {
  const total = Math.max(0, Math.round(ms));
  const m = Math.floor(total / 60_000);
  const s = Math.floor((total % 60_000) / 1_000);
  const rem = total % 1_000;
  const parts: string[] = [];
  if (m) parts.push(`${m}m`);
  if (s) parts.push(`${s}s`);
  if (rem || parts.length === 0) parts.push(`${rem}ms`);
  return parts.join(" ");
}

type BenchRow = {
  op: string;
  lib: number | null;
  ours: number | null;
};

type PageHit = {
  file: string;
  page: number;
  lib: number;
  ours: number;
  diff: number;
  pct: number;
};

function parseMs(s: string): number | null {
  if (s === "ERROR") return null;
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

/** Parse the comparison table printed by djvu_test -bench. */
function parseBenchStdout(stdout: string): { rows: BenchRow[]; extra: string[] } {
  const lines = stdout.split(/\r?\n/);
  const rows: BenchRow[] = [];
  const extra: string[] = [];
  let inTable = false;

  for (const line of lines) {
    if (!line) continue;
    const parts = line.trim().split(/\s+/);
    if (!inTable) {
      if (parts[0] === "op" && parts.includes("libdjvu") && parts.includes("djvudec")) {
        inTable = true;
        continue;
      }
      extra.push(line);
      continue;
    }
    // Table rows: op libdjvu djvudec diff %diff  (diff/%diff may be ERROR)
    if (parts.length >= 3) {
      const op = parts[0]!;
      if (op === "open" || op === "close" || op === "total" || /^\d+$/.test(op)) {
        rows.push({ op, lib: parseMs(parts[1]!), ours: parseMs(parts[2]!) });
        continue;
      }
    }
    // Past the table (e.g. document alloc line)
    extra.push(line);
  }
  return { rows, extra };
}

function fmtMs(n: number | null): string {
  return n === null ? "ERROR" : n.toFixed(2);
}

function fmtDiff(ours: number | null, lib: number | null): string {
  if (ours === null || lib === null) return "ERROR";
  return `${ours - lib >= 0 ? "+" : ""}${(ours - lib).toFixed(2)}`;
}

function fmtPct(ours: number | null, lib: number | null): string {
  if (ours === null || lib === null) return "ERROR";
  if (lib > 0) {
    const p = ((ours - lib) / lib) * 100;
    return `${p >= 0 ? "+" : ""}${p.toFixed(1)}%`;
  }
  return "0.0%";
}

function printTable(rows: BenchRow[]): void {
  const h = { op: "op", lib: "libdjvu", ours: "djvudec", diff: "diff", pct: "%diff" };
  const cells = rows.map((r) => ({
    op: r.op,
    lib: fmtMs(r.lib),
    ours: fmtMs(r.ours),
    diff: fmtDiff(r.ours, r.lib),
    pct: fmtPct(r.ours, r.lib),
  }));
  let wOp = h.op.length,
    wLib = h.lib.length,
    wOurs = h.ours.length,
    wDiff = h.diff.length,
    wPct = h.pct.length;
  for (const c of cells) {
    wOp = Math.max(wOp, c.op.length);
    wLib = Math.max(wLib, c.lib.length);
    wOurs = Math.max(wOurs, c.ours.length);
    wDiff = Math.max(wDiff, c.diff.length);
    wPct = Math.max(wPct, c.pct.length);
  }
  const line = (op: string, lib: string, ours: string, diff: string, pct: string) =>
    `${op.padEnd(wOp)} ${lib.padStart(wLib)} ${ours.padStart(wOurs)} ${diff.padStart(wDiff)} ${pct.padStart(wPct)}`;
  console.log(line(h.op, h.lib, h.ours, h.diff, h.pct));
  for (const c of cells) console.log(line(c.op, c.lib, c.ours, c.diff, c.pct));
}

const benchFlag = likeSumatra ? "-bench-sum" : "-bench";
let rc = 0;
const pageHits: PageHit[] = [];
const t0 = performance.now();

for (const file of files) {
  const label = fileLabel(file, ROOT);
  if (files.length > 1) console.log(`\n=== ${label}`);

  const r = Bun.spawnSync({
    cmd: [TEST, benchFlag, file],
    stdout: "pipe",
    stderr: "inherit",
  });
  if (r.exitCode) rc = r.exitCode;

  const stdout = r.stdout.toString();
  const { rows, extra } = parseBenchStdout(stdout);

  if (rows.length === 0) {
    // Fallback: show raw output if the table wasn't parseable.
    process.stdout.write(stdout);
    continue;
  }

  for (const row of rows) {
    if (!/^\d+$/.test(row.op) || row.lib === null || row.ours === null) continue;
    const diff = row.ours - row.lib;
    const pct = row.lib > 0 ? (diff / row.lib) * 100 : 0;
    pageHits.push({
      file: label,
      page: Number(row.op),
      lib: row.lib,
      ours: row.ours,
      diff,
      pct,
    });
  }

  if (verbose) {
    printTable(rows);
    for (const line of extra) console.log(line);
  } else {
    const total = rows.find((r) => r.op === "total");
    printTable(total ? [total] : []);
  }
}

console.log(`elapsed ${formatElapsed(performance.now() - t0)}`);

pageHits.sort((a, b) => b.diff - a.diff || b.pct - a.pct);
const top = pageHits.slice(0, 10);
if (top.length > 0) {
  console.log(`\ntop ${top.length} slowest pages vs libdjvu (+ = djvudec slower):`);
  const hFile = "file",
    hPage = "page",
    hLib = "libdjvu",
    hOurs = "djvudec",
    hDiff = "diff",
    hPct = "%diff";
  let wFile = hFile.length,
    wPage = hPage.length,
    wLib = hLib.length,
    wOurs = hOurs.length,
    wDiff = hDiff.length,
    wPct = hPct.length;
  const cells = top.map((p) => ({
    file: p.file,
    page: String(p.page),
    lib: p.lib.toFixed(2),
    ours: p.ours.toFixed(2),
    diff: `${p.diff >= 0 ? "+" : ""}${p.diff.toFixed(2)}`,
    pct: `${p.pct >= 0 ? "+" : ""}${p.pct.toFixed(1)}%`,
  }));
  for (const c of cells) {
    wFile = Math.max(wFile, c.file.length);
    wPage = Math.max(wPage, c.page.length);
    wLib = Math.max(wLib, c.lib.length);
    wOurs = Math.max(wOurs, c.ours.length);
    wDiff = Math.max(wDiff, c.diff.length);
    wPct = Math.max(wPct, c.pct.length);
  }
  console.log(
    `${hFile.padEnd(wFile)} ${hPage.padStart(wPage)} ${hLib.padStart(wLib)} ${hOurs.padStart(wOurs)} ${hDiff.padStart(wDiff)} ${hPct.padStart(wPct)}`,
  );
  for (const c of cells) {
    console.log(
      `${c.file.padEnd(wFile)} ${c.page.padStart(wPage)} ${c.lib.padStart(wLib)} ${c.ours.padStart(wOurs)} ${c.diff.padStart(wDiff)} ${c.pct.padStart(wPct)}`,
    );
  }
}

process.exit(rc);
