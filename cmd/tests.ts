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
// with length-prefixed per-page djvutxt on stdin. Runs over every .djvu under
// testfiles/subset by default (`-full` uses testfiles/full); set DJVU_SPECS to
// override. See test/file_features.md for the curated subset. Files are tested
// in parallel across one worker per CPU; `-cpu N`
// overrides the count.
// `-rand N` limits the run to N randomly chosen files from the corpus.
// `-failout path` writes failing file paths (default: failures.txt in repo root);
// each failure is appended as soon as it is found.
// `-failures path` tests only paths listed in that file (one per line, # comments).
// `-clean` deletes out/ before building, forcing a full harness rebuild.
// After each djvu_test_* subprocess exits, Windows FFI enumerates processes; if
// any djvu_test_* process exceeds 8 GB RAM, all are killed and the offending
// file is printed before exit. Long books are verified in VERIFY_PAGE_CHUNK-page
// subprocesses; render and text verify run sequentially per file.
import { appendFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "fs";
import { cpus } from "os";
import { join, dirname, basename } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build, buildAsan, cleanBuildOutput, defaultUseClang, refToolPath } from "./build";
import { corpusDir } from "./corpus";
import { trackDjvuTestProc, awaitDjvuTestProc } from "./win_proc_mem";

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

async function fileMeta(
  f: string,
  data: Buffer,
): Promise<{ nPages: number; anno: boolean; text: boolean }> {
  const offs = pageOffsets(data);
  if (offs.length > 0) return { nPages: offs.length, ...docFeatures(data, offs) };
  const out = await run([TEST, "-info", f], f);
  const pages = parseInt(out.toString("latin1").match(/pages: (\d+)/)?.[1] ?? "0", 10);
  return { nPages: pages, ...scanChunkTags(data) };
}

// Run djvu_test and capture stdout; checks for runaway memory after exit (Windows FFI).
async function run(cmd: string[], file: string): Promise<Buffer> {
  const proc = Bun.spawn({ cmd, stdout: "pipe", stderr: "ignore" });
  trackDjvuTestProc(proc, file, pidToFile);
  const out = Buffer.from(await new Response(proc.stdout).arrayBuffer());
  const code = await awaitDjvuTestProc(proc, pidToFile);
  if (code !== 0) return Buffer.alloc(0);
  return out;
}

type VerifyResult = {
  fRender: number[];
  fText: number[];
  m: number;
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
};

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

// Parse `djvu_test -verify-render` tab-separated output.
function parseVerifyRender(
  name: string,
  out: Buffer,
): Pick<
  VerifyResult,
  "fRender" | "m" | "mm" | "skip" | "bad" | "memTotal" | "memPeak" | "memAllocs" | "memLeak"
> {
  const fRender: number[] = [];
  const bad: string[] = [];
  let m = 0,
    mm = 0,
    skip = 0,
    memTotal = 0,
    memPeak = 0,
    memAllocs = 0,
    memLeak = false;

  for (const line of out.toString("latin1").split(/\r?\n/)) {
    if (!line) continue;
    const parts = line.split("\t");
    const kind = parts[0];
    const page = parseInt(parts[1], 10);
    const status = parts[2];
    if (kind === "memstat") {
      // memstat <total> <peak> <allocs> <frees> <live>
      memTotal += parseInt(parts[1], 10) || 0;
      memPeak = Math.max(memPeak, parseInt(parts[2], 10) || 0);
      memAllocs += parseInt(parts[3], 10) || 0;
      if ((parseInt(parts[5], 10) || 0) > 0) memLeak = true;
    } else if (kind === "render") {
      if (status === "ok") m++;
      else if (status === "skip") skip++;
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
      m = parseInt(parts[4], 10) || m;
      mm = parseInt(parts[5], 10) || mm;
      skip = parseInt(parts[6], 10) || skip;
    }
  }
  return { fRender, m, mm, skip, bad, memTotal, memPeak, memAllocs, memLeak };
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
      pages.map((p) => run([refToolPath("djvutxt"), `--page=${p}`, f])),
    );
    for (const chunk of chunks) {
      const hdr = Buffer.alloc(4);
      hdr.writeUInt32BE(chunk.length, 0);
      packed.push(hdr, chunk);
    }
  }
  return Buffer.concat(packed);
}

// In-memory render verify via djvu_test -verify-render (ddjvuapi oracle inside).
async function verifyRender(
  f: string,
  name: string,
  nPages: number,
): Promise<Pick<VerifyResult, "fRender" | "m" | "mm" | "skip" | "bad">> {
  const diffDir = join(VERIFY_DIFFS, safeDirName(name));
  mkdirSync(diffDir, { recursive: true });

  const merged = {
    fRender: [] as number[],
    m: 0,
    mm: 0,
    skip: 0,
    bad: [] as string[],
    memTotal: 0,
    memPeak: 0,
    memAllocs: 0,
    memLeak: false,
  };

  for (let lo = 1; lo <= nPages; lo += VERIFY_PAGE_CHUNK) {
    const hi = Math.min(nPages, lo + VERIFY_PAGE_CHUNK - 1);
    const proc = Bun.spawn({
      cmd: [TEST, "-verify-render", "-diffdir", diffDir, f],
      stdout: "pipe",
      stderr: ASAN ? "pipe" : "ignore",
      env: {
        ...process.env,
        DJVU_VERIFY_LO: String(lo),
        DJVU_VERIFY_HI: String(hi),
        ...asanEnv(),
      },
    });
    trackDjvuTestProc(proc, f, pidToFile);
    // read stdout and (asan) stderr concurrently to avoid pipe-buffer deadlock
    const [out, errTxt] = await Promise.all([
      new Response(proc.stdout).arrayBuffer().then((b) => Buffer.from(b)),
      ASAN ? new Response(proc.stderr!).text() : Promise.resolve(""),
    ]);
    const code = await awaitDjvuTestProc(proc, pidToFile);
    if (ASAN && code === ASAN_EXITCODE) {
      return {
        fRender: [] as number[],
        m: 0,
        mm: 1,
        skip: 0,
        bad: [`${name}: AddressSanitizer error (p${lo}-${hi})\n${asanReport(errTxt)}`],
        memTotal: 0,
        memPeak: 0,
        memAllocs: 0,
        memLeak: false,
      };
    }
    if (code === 3) {
      return {
        fRender: [] as number[],
        m: 0,
        mm: 1,
        skip: 0,
        bad: [`${name}: djvu_test -verify-render p${lo}-${hi} exceeded the memory limit (4 GB per ctx / DJVU_VERIFY_MEM_MB)`],
        memTotal: 0,
        memPeak: 0,
        memAllocs: 0,
        memLeak: false,
      };
    }
    if (code !== 0 && code !== 1) {
      return {
        fRender: [] as number[],
        m: 0,
        mm: 1,
        skip: 0,
        bad: [`${name}: djvu_test -verify-render p${lo}-${hi} exited ${code}`],
        memTotal: 0,
        memPeak: 0,
        memAllocs: 0,
        memLeak: false,
      };
    }
    const part = parseVerifyRender(name, out);
    merged.m += part.m;
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
    };
  }
  if (code !== 0 && code !== 1) {
    return {
      fText: [] as number[],
      tm: 0,
      tmm: 1,
      te: 0,
      tbad: [`${name}: djvu_test -verify-text exited ${code}`],
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

// Every .djvu under dir, recursively (sorted by path).
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

async function main(): Promise<number> {
  await getDeps();
  const SPECS = corpusDir(ROOT);
  ASAN = process.argv.includes("-asan");
  const useClang = process.argv.includes("-clang") || defaultUseClang;
  if (process.argv.includes("-clean")) cleanBuildOutput();
  await buildRef();
  // -asan: verify under a clang + AddressSanitizer build to catch memory bugs.
  TEST = ASAN ? await buildAsan() : await build(useClang);
  if (ASAN) console.log("running under AddressSanitizer (out/clang_asan)");

  let m = 0,
    mm = 0,
    skip = 0;
  let tm = 0,
    tmm = 0,
    te = 0;
  const bad: string[] = [];
  const tbad: string[] = [];
  const failedFiles = new Map<string, string[]>();

  const failOutPath = argPath("-failout") ?? join(ROOT, "failures.txt");
  const failInPath = argPath("-failures");

  let files: string[];
  let totalFiles: number;
  if (failInPath) {
    if (!existsSync(failInPath)) {
      console.error(`failures list not found: ${failInPath}`);
      return 1;
    }
    files = readFailureList(failInPath);
    totalFiles = files.length;
    console.log(`testing ${files.length} files from ${failInPath}`);
  } else {
    files = walkDjvu(SPECS).sort();
    totalFiles = files.length;
  }

  const randArg = process.argv.indexOf("-rand");
  if (randArg >= 0) {
    const nRand = parseInt(process.argv[randArg + 1]);
    if (nRand > 0 && nRand < files.length) {
      const shuffled = [...files];
      for (let i = shuffled.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
      }
      files = shuffled.slice(0, nRand);
    } else if (nRand > 0) {
      files = files.slice(0, nRand);
    }
  }

  const cpuArg = process.argv.indexOf("-cpu");
  const nWorkers = Math.max(
    1,
    Math.min(
      cpuArg >= 0 ? parseInt(process.argv[cpuArg + 1]) || 1 : cpus().length,
      files.length || 1,
    ),
  );
  const randNote =
    randArg >= 0 && files.length < totalFiles
      ? ` (${files.length} random of ${totalFiles})`
      : "";
  console.log(`corpus: ${SPECS}`);
  console.log(`testing ${files.length} files${randNote} with ${nWorkers} workers`);

  writeFileSync(failOutPath, "");

  let appendChain = Promise.resolve();
  const appendFailure = (path: string, diffs: string[]) => {
    const block = path + "\n" + diffs.map((d) => `# ${d}`).join("\n") + "\n";
    appendChain = appendChain.then(() => appendFileSync(failOutPath, block));
    return appendChain;
  };

  let finished = 0;
  async function testFile(f: string) {
    const t0 = performance.now();
    const data = readFileSync(f);
    const { nPages, anno, text: hasText } = await fileMeta(f, data);
    const feats = [`${nPages} pages`];
    if (anno) feats.push("annots");
    if (hasText) feats.push("text");
    const vr = await verifyFile(f, nPages, hasText);
    m += vr.m;
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

  console.log(`render (mask=pgm, bg/color=ppm): MATCH=${m} MISMATCH=${mm}; skipped=${skip}`);
  for (const x of bad.slice(0, 50)) console.log("  render MISMATCH", x);
  console.log(`text: MATCH=${tm} MISMATCH=${tmm}; both-empty=${te}`);
  for (const x of tbad.slice(0, 50)) console.log("  text MISMATCH", x);

  await appendChain;
  if (failedFiles.size)
    console.log(`${failedFiles.size} failing file(s) in ${failOutPath}`);
  else
    console.log(`no failures (${failOutPath} cleared)`);

  return mm || tmm ? 1 : 0;
}

process.exit(await main());