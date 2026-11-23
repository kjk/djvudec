#!/usr/bin/env bun
// tests.ts -- verify djvu_test render output against DjVuLibre's ddjvu, page
// by page (run with `bun cmd/tests.ts`; replaces the old Python verify.py).
//
// This is the test entry point: it ensures deps (get-deps.ts), builds the ref
// tools + harness (buildRef/build from build.ts), then verifies.
//
// Pages that are pure JB2 masks (Sjbz, no BG44/FG44 background) must match
// `ddjvu -format=pgm` byte-for-byte. Pages with an IW44 background or color are
// compared as ppm. Text is compared against djvutxt (trailing separators
// ignored). Renders use one `djvu_test -verify-render` per file (in-memory bitmap
// compare vs ddjvuapi; PNMs written to verify_diffs/ only on mismatch). Verify
// decodes IW44 per page (lazy) and opens a fresh ddjvuapi document per page so
// multipage books do not retain every layer in RAM. Text uses
// one `djvu_test -verify-text` per file (doc opened once)
// with length-prefixed per-page djvutxt on stdin. The corpus is the .djvu
// files inside the deps/ reference checkouts (cmd/corpus.ts; DJVU_SPECS
// overrides with any directory). With no selection argument it prints usage
// and the available file count; run with `-all`, `-rand N`, explicit files,
// or `-failures path` (selection is corpus.ts selectFiles, the same convention
// as every other script, plus `-failures`). Files are tested in parallel across
// one worker per CPU; `-cpu N` overrides the count.
// `-failout path` writes failing file paths (default: failures.txt in repo root);
// each failure is appended as soon as it is found.
// A file our decoder cannot open (or that cannot be read) is a FAILURE; a file
// that opens with 0 pages is not. Pages the harness reports as
// `ok palette-overlap` (differ from ddjvu only where DjVuLibre adds the colours
// of overlapping palette blits; see image_compare in test/djvu_test.c) count as
// matches and are shown separately: `MATCH=N (palette-overlap=K)`. An empty verify_diffs/<name>/
// is removed again when a file produced no mismatch dumps.
// `-clean` deletes out/ before building, forcing a full harness rebuild.
// After each djvu_test_* subprocess exits, Windows FFI enumerates processes; if
// any djvu_test_* process exceeds 8 GB RAM, all are killed and the offending
// file is printed before exit. Long books are verified in VERIFY_PAGE_CHUNK-page
// subprocesses; render and text verify run sequentially per file.
import { appendFileSync, existsSync, mkdirSync, readFileSync, rmdirSync, writeFileSync } from "fs";
import { cpus } from "os";
import { join, dirname, basename } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build, buildAsan, cleanBuildOutput, refToolPath } from "./build";
import { corpusSummary, selectFiles } from "./corpus";
import { defaultUseClang } from "./util";
import { trackDjvuTestProc, awaitDjvuTestProc } from "./win-proc-mem";

const ROOT = dirname(import.meta.dir);
const VERIFY_DIFFS = join(ROOT, "verify_diffs");
let TEST = ""; // set by build() / buildAsan() in main()
const pidToFile = new Map<number, string>();

// -asan: run djvu_test under AddressSanitizer. We give ASan a sentinel exit code
// (its default is 1, which we treat as "completed with mismatches") and capture
// stderr so a detected error surfaces as a file failure instead of being absorbed.
let ASAN = false;
const ASAN_EXITCODE = 21;
const asanEnv = () => (ASAN ? { ASAN_OPTIONS: `exitcode=${ASAN_EXITCODE}:halt_on_error=1` } : {});
function asanReport(stderr: string): string {
  const m = stderr.match(/==\d+==ERROR: AddressSanitizer[\s\S]*?SUMMARY: AddressSanitizer[^\n]*/);
  return m ? m[0] : stderr.slice(-1500).trim();
}

// Fresh djvu_test subprocess every N pages so CRT/libdjvu working set does not
// climb on long multipage -verify-render runs (e.g. 360-page books).
const VERIFY_PAGE_CHUNK = 16;

const tag = (d: Buffer, p: number) => d.toString("latin1", p, p + 4);

// File offsets of every displayable page FORM (FORM:DJVU) in a bundled DJVM.
// Matches DIRM layout in document.c (flag byte, u16 count, bundled u32 offsets).
function pageOffsets(data: Buffer): number[] {
  let p = tag(data, 0) === "AT&T" ? 4 : 0;
  if (p + 12 > data.length || tag(data, p) !== "FORM") return [];
  if (tag(data, p + 8) === "DJVU") return [p];
  const end = Math.min(p + 8 + data.readUInt32BE(p + 4), data.length);
  let q = p + 12;
  while (q + 8 <= end) {
    const cid = tag(data, q);
    const csz = data.readUInt32BE(q + 4);
    const chunkEnd = q + 8 + csz;
    if (chunkEnd > end) break;
    if (cid === "DIRM") {
      const d = q + 8;
      const dirmEnd = Math.min(chunkEnd, data.length);
      if (d + 3 > dirmEnd) return [];
      const bundled = (data[d] >> 7) & 1;
      const cnt = data.readUInt16BE(d + 1);
      if (cnt <= 0 || cnt > 100000) return [];
      if (!bundled) return []; /* indirect/shared bundle: use -info for page count */
      const offBase = d + 3;
      if (offBase + cnt * 4 > dirmEnd || offBase + cnt * 4 > data.length) return [];
      const offs: number[] = [];
      for (let i = 0; i < cnt; i++) {
        const o = data.readUInt32BE(offBase + i * 4);
        if (o + 12 <= data.length && tag(data, o) === "FORM" && tag(data, o + 8) === "DJVU")
          offs.push(o);
      }
      return offs;
    }
    q = chunkEnd + (csz & 1);
  }
  return [];
}

// Whether page FORMs carry annotation or text chunks (progress-line hint).
function docFeatures(data: Buffer, offs: number[]): { anno: boolean; text: boolean } {
  let anno = false,
    text = false;
  for (const off of offs) {
    if (off + 12 > data.length) continue;
    const end = Math.min(off + 8 + data.readUInt32BE(off + 4), data.length);
    let p = off + 12;
    while (p + 8 <= end) {
      const cid = tag(data, p);
      const csz = data.readUInt32BE(p + 4);
      const chunkEnd = p + 8 + csz;
      if (chunkEnd > end) break;
      if (cid === "ANTa" || cid === "ANTz") anno = true;
      if (cid === "TXTa" || cid === "TXTz") text = true;
      p = chunkEnd + (csz & 1);
    }
  }
  return { anno, text };
}

// Fallback when DIRM offsets are not inline (indirect bundle, parse failure).
function scanChunkTags(data: Buffer): { anno: boolean; text: boolean } {
  let anno = false,
    text = false;
  for (let i = 0; i + 4 <= data.length; i++) {
    const cid = tag(data, i);
    if (cid === "ANTa" || cid === "ANTz") anno = true;
    if (cid === "TXTa" || cid === "TXTz") text = true;
  }
  return { anno, text };
}

// Page count + feature hints for one file. openError is set when our decoder
// cannot open the file at all: `djvu_test -info` prints "pages: N" and exits 0
// for any document it opens (N may legitimately be 0), and exits non-zero
// ("cannot open document") otherwise -- so the exit code, not N, tells an
// unopenable file (a failure) from a genuinely page-less one (not a failure).
async function fileMeta(
  f: string,
  data: Buffer,
): Promise<{ nPages: number; anno: boolean; text: boolean; openError?: string }> {
  const offs = pageOffsets(data);
  if (offs.length > 0) return { nPages: offs.length, ...docFeatures(data, offs) };
  const { code, out } = await runWithCode([TEST, "-info", f], f);
  const feats = scanChunkTags(data);
  if (code !== 0) {
    return { nPages: 0, ...feats, openError: `cannot open (djvu_test -info exited ${code})` };
  }
  const m = out.toString("latin1").match(/pages: (\d+)/);
  if (!m) {
    return { nPages: 0, ...feats, openError: "cannot open (djvu_test -info printed no page count)" };
  }
  return { nPages: parseInt(m[1], 10), ...feats };
}

// Run a tool and capture stdout + exit code; checks for runaway memory after
// exit (Windows FFI).
async function runWithCode(cmd: string[], file: string): Promise<{ code: number; out: Buffer }> {
  const proc = Bun.spawn({ cmd, stdout: "pipe", stderr: "ignore" });
  trackDjvuTestProc(proc, file, pidToFile);
  const out = Buffer.from(await new Response(proc.stdout).arrayBuffer());
  const code = await awaitDjvuTestProc(proc, pidToFile);
  return { code, out };
}

// Same, for callers that only want the output: empty on a non-zero exit.
async function run(cmd: string[], file: string): Promise<Buffer> {
  const { code, out } = await runWithCode(cmd, file);
  return code !== 0 ? Buffer.alloc(0) : out;
}

type VerifyResult = {
  fRender: number[];
  fText: number[];
  m: number; // pages that match the oracle (includes the overlap ones)
  overlap: number; // of m: differ from ddjvu only by its additive palette blend
  mm: number;
  skip: number;
  tm: number;
  tmm: number;
  te: number;
  bad: string[];
  tbad: string[];
  memTotal: number; // bytes ever allocated for the file (sum over page chunks)
  memPeak: number; // peak live bytes for a single ctx (max over chunks)
  memAllocs: number; // number of allocations (sum over page chunks)
  memLeak: boolean; // a chunk reported unfreed allocations
  renderCrashed?: boolean; // djvu_test -verify-render died (no per-page result)
  renderNoResult?: boolean; // -verify-render exited without a summary (cannot open)
  textCrashed?: boolean; // djvu_test -verify-text died
};

// The render-verify fields of a VerifyResult.
type RenderResult = Pick<
  VerifyResult,
  | "fRender" | "m" | "overlap" | "mm" | "skip" | "bad"
  | "memTotal" | "memPeak" | "memAllocs" | "memLeak"
  | "renderCrashed" | "renderNoResult"
>;

// A whole-file render failure (crash, memory limit, ASan, no result): one
// mismatch carrying the message, no per-page data.
function renderFailure(msg: string, flag: "renderCrashed" | "renderNoResult"): RenderResult {
  return {
    fRender: [], m: 0, overlap: 0, mm: 1, skip: 0, bad: [msg],
    memTotal: 0, memPeak: 0, memAllocs: 0, memLeak: false, [flag]: true,
  };
}

// Human-readable byte count, e.g. 52279209 -> "49.86 MB".
function humanBytes(n: number): string {
  const u = ["B", "KB", "MB", "GB", "TB"];
  let b = n,
    i = 0;
  while (b >= 1024 && i < u.length - 1) {
    b /= 1024;
    i++;
  }
  return `${i === 0 ? b.toFixed(0) : b.toFixed(2)} ${u[i]}`;
}

// Parse `djvu_test -verify-text` tab-separated output.
function parseVerifyText(name: string, out: Buffer): Pick<VerifyResult, "fText" | "tm" | "tmm" | "te" | "tbad"> {
  const fText: number[] = [];
  const tbad: string[] = [];
  let tm = 0,
    tmm = 0,
    te = 0;

  for (const line of out.toString("latin1").split(/\r?\n/)) {
    if (!line) continue;
    const parts = line.split("\t");
    const kind = parts[0];
    const page = parseInt(parts[1], 10);
    const status = parts[2];
    if (kind === "text") {
      if (status === "ok") tm++;
      else if (status === "empty") te++;
      else {
        tmm++;
        fText.push(page);
        tbad.push(`${name} p${page}`);
      }
    } else if (kind === "summary") {
      tm = parseInt(parts[4], 10) || tm;
      tmm = parseInt(parts[5], 10) || tmm;
      te = parseInt(parts[6], 10) || te;
    }
  }
  return { fText, tm, tmm, te, tbad };
}

function safeDirName(name: string): string {
  return name.replace(/[<>:"/\\|?*]/g, "_");
}

// Parse `djvu_test -verify-render` tab-separated output:
//   render <page> <status> [extra...]     one per page
//   memstat <total> <peak> <allocs> <frees> <live>
//   summary 0 0 0 <match> <mismatch> <skip>   always last when the doc opened
// A page matches when its status is `ok` (optionally followed by extra
// fields). `ok palette-overlap <n>` means "differs from ddjvu only by
// DjVuLibre's additive blend at overlapping palette blits"; such pages are
// counted in `overlap` as well as in `m`. Counts come from the per-page lines; the
// summary line is only used as a fallback (and as proof the run completed).
function parseVerifyRender(
  name: string,
  out: Buffer,
): RenderResult & { sawSummary: boolean } {
  const fRender: number[] = [];
  const bad: string[] = [];
  let m = 0,
    overlap = 0,
    mm = 0,
    skip = 0,
    memTotal = 0,
    memPeak = 0,
    memAllocs = 0,
    memLeak = false,
    sawSummary = false,
    pageLines = 0;

  for (const line of out.toString("latin1").split(/\r?\n/)) {
    if (!line) continue;
    const parts = line.split("\t");
    const kind = parts[0];
    const page = parseInt(parts[1], 10);
    const status = parts[2] ?? "";
    if (kind === "memstat") {
      // memstat <total> <peak> <allocs> <frees> <live>
      memTotal += parseInt(parts[1], 10) || 0;
      memPeak = Math.max(memPeak, parseInt(parts[2], 10) || 0);
      memAllocs += parseInt(parts[3], 10) || 0;
      if ((parseInt(parts[5], 10) || 0) > 0) memLeak = true;
    } else if (kind === "render") {
      pageLines++;
      if (status.startsWith("ok")) {
        m++;
        if (parts.slice(3).includes("palette-overlap")) overlap++;
      } else if (status === "skip") skip++;
      else {
        mm++;
        fRender.push(page);
        const refPath = parts[3] ?? "";
        const minePath = parts[4] ?? "";
        bad.push(
          refPath && minePath
            ? `${name} p${page} ref=${refPath} mine=${minePath}`
            : `${name} p${page} (${status})`,
        );
      }
    } else if (kind === "summary") {
      sawSummary = true;
      if (pageLines === 0) {
        m = parseInt(parts[4], 10) || 0;
        mm = parseInt(parts[5], 10) || 0;
        skip = parseInt(parts[6], 10) || 0;
      }
    }
  }
  return { fRender, m, overlap, mm, skip, bad, memTotal, memPeak, memAllocs, memLeak, sawSummary };
}

// Per-page djvutxt packed for -verify-text: u32-BE len + bytes per page.
async function fetchRefText(f: string, nPages: number): Promise<Buffer> {
  const packed: Buffer[] = [];
  const batch = 16;
  for (let start = 1; start <= nPages; start += batch) {
    const end = Math.min(nPages, start + batch - 1);
    const pages: number[] = [];
    for (let p = start; p <= end; p++) pages.push(p);
    const chunks = await Promise.all(
      pages.map((p) => run([refToolPath("djvutxt"), `--page=${p}`, f], f)),
    );
    for (const chunk of chunks) {
      const hdr = Buffer.alloc(4);
      hdr.writeUInt32BE(chunk.length, 0);
      packed.push(hdr, chunk);
    }
  }
  return Buffer.concat(packed);
}

// Bun reports only the low byte of a Windows exit status, so an NTSTATUS crash
// shows up as e.g. 9 (0xC0000409 fast-fail: abort, uncaught C++ exception,
// /GS failure). Name the likely status, the last page djvu_test finished
// (stdout is flushed per page) and its `crash` breadcrumb from stderr.
const WIN_CRASH_LOW_BYTE: Record<number, string> = {
  5: "0xC0000005 access violation",
  9: "0xC0000409 fast-fail (abort / uncaught exception / stack cookie)",
  23: "0xC0000017 out of memory",
  253: "0xC00000FD stack overflow",
};

function crashDetail(code: number, out: Buffer, errTxt: string): string {
  const parts = [`exited ${code}`];
  const status = process.platform === "win32" ? WIN_CRASH_LOW_BYTE[code] : undefined;
  if (status) parts.push(`(likely ${status})`);
  const done = out
    .toString("latin1")
    .split(/\r?\n/)
    .filter((l) => l.startsWith("render\t"));
  parts.push(done.length ? `after p${done[done.length - 1].split("\t")[1]}` : "before any page finished");
  const crash = errTxt.split(/\r?\n/).filter((l) => l.startsWith("crash\t")).pop();
  if (crash) {
    const [, page, phase, what, ntstatus] = crash.split("\t");
    parts.push(`[${what} ${ntstatus} on p${page} in ${phase}]`);
  }
  return parts.join(" ");
}

// In-memory render verify via djvu_test -verify-render (ddjvuapi oracle inside).
async function verifyRender(f: string, name: string, nPages: number): Promise<RenderResult> {
  // djvu_test writes ref/mine PNMs here only for mismatching pages. The
  // directory has to exist up front (the harness mkdirs one level only); drop
  // it again afterwards if nothing was written, so a passing run leaves no
  // empty verify_diffs/<name>/ behind.
  const diffDir = join(VERIFY_DIFFS, safeDirName(name));
  mkdirSync(diffDir, { recursive: true });
  try {
    return await verifyRenderChunks(f, name, nPages, diffDir);
  } finally {
    removeIfEmpty(diffDir);
  }
}

// rmdir fails on a non-empty (or missing) directory: exactly "remove if empty".
function removeIfEmpty(dir: string): void {
  try {
    rmdirSync(dir);
  } catch {}
}

async function verifyRenderChunks(
  f: string,
  name: string,
  nPages: number,
  diffDir: string,
): Promise<RenderResult> {
  const merged: RenderResult = {
    fRender: [],
    m: 0,
    overlap: 0,
    mm: 0,
    skip: 0,
    bad: [],
    memTotal: 0,
    memPeak: 0,
    memAllocs: 0,
    memLeak: false,
  };

  const runChunk = async (lo: number, hi: number) => {
    const proc = Bun.spawn({
      cmd: [TEST, "-verify-render", "-diffdir", diffDir, f],
      stdout: "pipe",
      stderr: "pipe",
      env: {
        ...process.env,
        DJVU_VERIFY_LO: String(lo),
        DJVU_VERIFY_HI: String(hi),
        ...asanEnv(),
      },
    });
    trackDjvuTestProc(proc, f, pidToFile);
    // read stdout and stderr concurrently to avoid pipe-buffer deadlock
    const [out, errTxt] = await Promise.all([
      new Response(proc.stdout).arrayBuffer().then((b) => Buffer.from(b)),
      new Response(proc.stderr).text(),
    ]);
    const code = await awaitDjvuTestProc(proc, pidToFile);
    return { code, out, errTxt };
  };

  for (let lo = 1; lo <= nPages; lo += VERIFY_PAGE_CHUNK) {
    const hi = Math.min(nPages, lo + VERIFY_PAGE_CHUNK - 1);
    const what = `djvu_test -verify-render p${lo}-${hi}`;
    let { code, out, errTxt } = await runChunk(lo, hi);
    if (ASAN && code === ASAN_EXITCODE) {
      return renderFailure(
        `${name}: AddressSanitizer error (p${lo}-${hi})\n${asanReport(errTxt)}`,
        "renderCrashed",
      );
    }
    if (code === 3) {
      return renderFailure(
        `${name}: ${what} exceeded the memory limit (4 GB per ctx / DJVU_VERIFY_MEM_MB)`,
        "renderCrashed",
      );
    }
    // exit 4 = finished but leaked; the memstat line already flags it
    if (code !== 0 && code !== 1 && code !== 4) {
      // A crash (e.g. a rare one inside the in-process ddjvu oracle) is retried
      // once: a real decoder crash is deterministic and fails again.
      const first = crashDetail(code, out, errTxt);
      console.log(`  CRASH ${name}: ${what} ${first}; retrying`);
      ({ code, out, errTxt } = await runChunk(lo, hi));
      if (code !== 0 && code !== 1 && code !== 4) {
        const again = crashDetail(code, out, errTxt);
        return renderFailure(`${name}: ${what} ${first}; retry ${again}`, "renderCrashed");
      }
      console.log(`  CRASH ${name}: p${lo}-${hi} passed on retry (flaky)`);
    }
    const part = parseVerifyRender(name, out);
    // Exit 0/1/4 normally means "ran every page" -- but exit 1 is also how the
    // harness reports "cannot open document" (before any page, no summary
    // line). Without a summary there is no result to count: that is a failure,
    // not zero mismatches.
    if (!part.sawSummary) {
      const why = errTxt.trim().split(/\r?\n/).pop() || "no output";
      return renderFailure(
        `${name}: ${what} exited ${code} without a result (${why})`,
        "renderNoResult",
      );
    }
    merged.m += part.m;
    merged.overlap += part.overlap;
    merged.mm += part.mm;
    merged.skip += part.skip;
    merged.fRender.push(...part.fRender);
    merged.bad.push(...part.bad);
    merged.memTotal += part.memTotal;
    merged.memPeak = Math.max(merged.memPeak, part.memPeak);
    merged.memAllocs += part.memAllocs;
    merged.memLeak = merged.memLeak || part.memLeak;
  }
  return merged;
}

async function verifyText(f: string, nPages: number, hasText: boolean, name: string) {
  if (!hasText) {
    return { fText: [] as number[], tm: 0, tmm: 0, te: nPages, tbad: [] as string[] };
  }
  const refText = await fetchRefText(f, nPages);
  const proc = Bun.spawn({
    cmd: [TEST, "-verify-text", f],
    stdin: refText,
    stdout: "pipe",
    stderr: ASAN ? "pipe" : "ignore",
    env: { ...process.env, ...asanEnv() },
  });
  trackDjvuTestProc(proc, f, pidToFile);
  const [out, errTxt] = await Promise.all([
    new Response(proc.stdout).arrayBuffer().then((b) => Buffer.from(b)),
    ASAN ? new Response(proc.stderr!).text() : Promise.resolve(""),
  ]);
  const code = await awaitDjvuTestProc(proc, pidToFile);
  if (ASAN && code === ASAN_EXITCODE) {
    return {
      fText: [] as number[],
      tm: 0,
      tmm: 1,
      te: 0,
      tbad: [`${name}: AddressSanitizer error (verify-text)\n${asanReport(errTxt)}`],
      textCrashed: true,
    };
  }
  if (code !== 0 && code !== 1) {
    return {
      fText: [] as number[],
      tm: 0,
      tmm: 1,
      te: 0,
      tbad: [`${name}: djvu_test -verify-text exited ${code}`],
      textCrashed: true,
    };
  }
  return parseVerifyText(name, out);
}

async function verifyFile(
  f: string,
  nPages: number,
  hasText: boolean,
): Promise<VerifyResult> {
  const name = basename(f);
  const render = await verifyRender(f, name, nPages);
  const text = await verifyText(f, nPages, hasText, name);
  return { ...render, ...text };
}


// Human-readable duration, e.g. 5018.3 -> "5s 18.3ms", 65018 -> "1m 5s 18.0ms".
function humanMs(ms: number): string {
  const parts: string[] = [];
  let rem = ms;
  const h = Math.floor(rem / 3_600_000); rem -= h * 3_600_000;
  const m = Math.floor(rem / 60_000); rem -= m * 60_000;
  const s = Math.floor(rem / 1_000); rem -= s * 1_000;
  if (h) parts.push(`${h}h`);
  if (m) parts.push(`${m}m`);
  if (s) parts.push(`${s}s`);
  parts.push(`${rem.toFixed(1)}ms`);
  return parts.join(" ");
}

function argPath(flag: string): string | null {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : null;
}

// Paths from a failures list (full paths, one per line; # starts a comment).
function readFailureList(path: string): string[] {
  return readFileSync(path, "utf8")
    .split(/\r?\n/)
    .map((l) => l.replace(/#.*$/, "").trim())
    .filter((l) => l.length > 0);
}

// Flags whose next argument is a value, not a .djvu path (for selectFiles).
const VALUE_FLAGS = ["-rand", "-cpu", "-failout", "-failures"];

function usageText(): string {
  return (
    `usage: bun cmd/tests.ts <selection> [options]
selection (required; default prints this help):
  -all             verify every corpus file
  -rand N          verify N randomly chosen corpus files
  -failures path   verify only paths listed in that file (one per line, # comments)
  file.djvu ...    verify the given files
options:
  -clang           build the clang harness instead of MSVC
  -asan            verify under AddressSanitizer (out/clang_asan)
  -cpu N           worker count (default: one per CPU)
  -failout path    where to write failing paths (default: failures.txt)
  -clean           delete out/ before building

${corpusSummary()}`
  );
}

async function main(): Promise<number> {
  await getDeps();
  ASAN = process.argv.includes("-asan");
  const useClang = process.argv.includes("-clang") || defaultUseClang;

  const failOutPath = argPath("-failout") ?? join(ROOT, "failures.txt");
  const failInPath = argPath("-failures");

  // -failures path is tests.ts's own selection mode; everything else is the
  // shared convention (corpus.ts selectFiles: -all, else -rand N, else explicit
  // files; no selection prints the usage and exits 2).
  let files: string[];
  if (failInPath) {
    if (!existsSync(failInPath)) {
      console.error(`failures list not found: ${failInPath}`);
      return 1;
    }
    files = readFailureList(failInPath);
    console.log(`testing ${files.length} files from ${failInPath}`);
  } else {
    files = selectFiles(usageText(), VALUE_FLAGS);
  }

  // CI: drop known DjVuLibre oracle failures that are not our bugs (see
  // AGENTS.md). Local -all still reports them.
  // - 1998_compression.djvu p19: ddjvu three-layer-stencil quirk
  // - nasa001C.orig.png.djvu: ddjvu "Cannot decode page 1" on standalone PM44
  if (process.env.CI) {
    const ciSkip = new Set([
      "1998_compression.djvu",
      "nasa001c.orig.png.djvu",
    ]);
    const skipped: string[] = [];
    files = files.filter((f) => {
      const name = basename(f).toLowerCase();
      if (ciSkip.has(name)) {
        skipped.push(basename(f));
        return false;
      }
      return true;
    });
    if (skipped.length > 0) {
      console.log(
        `CI: skipping ${skipped.join(", ")} (known DjVuLibre oracle quirks, not decoder bugs)`,
      );
    }
  }

  if (process.argv.includes("-clean")) cleanBuildOutput();
  await buildRef();
  // -asan: verify under a clang + AddressSanitizer build to catch memory bugs.
  TEST = ASAN ? await buildAsan() : await build(useClang);
  if (ASAN) console.log("running under AddressSanitizer (out/clang_asan)");

  let m = 0,
    overlap = 0,
    mm = 0,
    skip = 0;
  let tm = 0,
    tmm = 0,
    te = 0;
  const bad: string[] = [];
  const tbad: string[] = [];
  const failedFiles = new Map<string, string[]>();

  const cpuArg = process.argv.indexOf("-cpu");
  const nWorkers = Math.max(
    1,
    Math.min(
      cpuArg >= 0 ? parseInt(process.argv[cpuArg + 1]) || 1 : cpus().length,
      files.length || 1,
    ),
  );
  console.log(`testing ${files.length} files with ${nWorkers} workers`);

  writeFileSync(failOutPath, "");

  let appendChain = Promise.resolve();
  const appendFailure = (path: string, diffs: string[]) => {
    const block = path + "\n" + diffs.map((d) => `# ${d}`).join("\n") + "\n";
    appendChain = appendChain.then(() => appendFileSync(failOutPath, block));
    return appendChain;
  };

  let finished = 0;
  // A file that could not be verified at all (unreadable, or our decoder cannot
  // open it): a FAILURE, never "(0 pages) ... same".
  async function failFile(f: string, t0: number, why: string) {
    failedFiles.set(f, [why]);
    await appendFailure(f, [why]);
    finished++;
    console.log(
      `[${finished}/${files.length}] ${basename(f)} — ` +
        `${humanMs(performance.now() - t0)} — FAILED: ${why}`,
    );
  }

  async function testFile(f: string) {
    const t0 = performance.now();
    let data: Buffer;
    try {
      data = readFileSync(f);
    } catch (e) {
      return failFile(f, t0, `cannot read file (${(e as { code?: string }).code ?? e})`);
    }
    const { nPages, anno, text: hasText, openError } = await fileMeta(f, data);
    if (openError) return failFile(f, t0, openError);
    const feats = [`${nPages} pages`];
    if (anno) feats.push("annots");
    if (hasText) feats.push("text");
    const vr = await verifyFile(f, nPages, hasText);
    m += vr.m;
    overlap += vr.overlap;
    mm += vr.mm;
    skip += vr.skip;
    tm += vr.tm;
    tmm += vr.tmm;
    te += vr.te;
    bad.push(...vr.bad);
    tbad.push(...vr.tbad);
    const diffs: string[] = [];
    if (vr.fRender.length) diffs.push(`render diff p${vr.fRender.join(",p")}`);
    if (vr.fText.length) diffs.push(`text diff p${vr.fText.join(",p")}`);
    if (vr.memLeak) diffs.push("memory leak");
    if (vr.renderCrashed) diffs.push("render crashed");
    if (vr.renderNoResult) diffs.push("cannot open (render verify produced no result)");
    if (vr.textCrashed) diffs.push("text crashed");
    const status = diffs.length ? diffs.join(", ") : "same";
    if (diffs.length) {
      failedFiles.set(f, diffs);
      await appendFailure(f, diffs);
    }
    finished++;
    const memNote = vr.memTotal
      ? ` — alloc ${humanBytes(vr.memTotal)} (${vr.memAllocs} allocs), peak ${humanBytes(vr.memPeak)}`
      : "";
    console.log(
      `[${finished}/${files.length}] ${basename(f)} (${feats.join(", ")}) — ` +
        `${humanMs(performance.now() - t0)} — ${status}${memNote}`,
    );
  }

  let next = 0;
  const worker = async () => {
    for (let idx = next++; idx < files.length; idx = next++) {
      await testFile(files[idx]);
    }
  };
  await Promise.all(Array.from({ length: nWorkers }, () => worker()));

  // No mismatch dumps were written at all -> don't leave an empty verify_diffs/.
  removeIfEmpty(VERIFY_DIFFS);

  // overlap: pages counted in MATCH that differ from ddjvu only by its additive
  // palette blend at overlapping blits (shown only when there are any).
  const overlapNote = overlap ? ` (palette-overlap=${overlap})` : "";
  console.log(
    `render (mask=pgm, bg/color=ppm): MATCH=${m}${overlapNote} MISMATCH=${mm}; skipped=${skip}`,
  );
  for (const x of bad.slice(0, 50)) console.log("  render MISMATCH", x);
  console.log(`text: MATCH=${tm} MISMATCH=${tmm}; both-empty=${te}`);
  for (const x of tbad.slice(0, 50)) console.log("  text MISMATCH", x);

  await appendChain;
  if (failedFiles.size)
    console.log(`${failedFiles.size} failing file(s) in ${failOutPath}`);
  else
    console.log(`no failures (${failOutPath} cleared)`);

  return mm || tmm || failedFiles.size ? 1 : 0;
}

process.exit(await main());