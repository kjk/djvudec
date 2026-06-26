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
// compare vs ddjvuapi; PNMs written to verify_diffs/ only on mismatch). Text uses
// one `djvu_test -verify-text` per file (doc opened once)
// with length-prefixed per-page djvutxt on stdin. Runs over every .djvu under testfiles/
// recursively; set the DJVU_SPECS env var to point the scan at a different
// directory. Files are tested in parallel across one worker per CPU; `-cpu N`
// overrides the count.
// `-rand N` limits the run to N randomly chosen files from the corpus.
// `-failout path` writes failing file paths (default: failures.txt in repo root);
// each failure is appended as soon as it is found.
// `-failures path` tests only paths listed in that file (one per line, # comments).
// `-clean` deletes out/ before building, forcing a full harness rebuild.
import { appendFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "fs";
import { cpus } from "os";
import { join, dirname, basename } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build, cleanBuildOutput, defaultUseClang } from "./build";

const ROOT = dirname(import.meta.dir);
const RB = join(ROOT, "ref_build");
const VERIFY_DIFFS = join(ROOT, "verify_diffs");
let TEST = join(ROOT, "out", "msvc", "djvu_test_msvc.exe"); // set by build() in main()

const tag = (d: Buffer, p: number) => d.toString("latin1", p, p + 4);

// File offsets of every displayable page FORM (FORM:DJVU).
function pageOffsets(data: Buffer): number[] {
  let p = tag(data, 0) === "AT&T" ? 4 : 0;
  if (tag(data, p) !== "FORM") return [];
  if (tag(data, p + 8) === "DJVU") return [p];
  const end = p + 8 + data.readUInt32BE(p + 4);
  let q = p + 12;
  while (q + 8 <= end) {
    const cid = tag(data, q);
    const csz = data.readUInt32BE(q + 4);
    if (cid === "DIRM") {
      const d = q + 8;
      const cnt = data.readUInt16BE(d + 1);
      const offs: number[] = [];
      for (let i = 0; i < cnt; i++) offs.push(data.readUInt32BE(d + 3 + i * 4));
      return offs.filter(
        (o) => tag(data, o) === "FORM" && tag(data, o + 8) === "DJVU",
      );
    }
    q += 8 + csz + (csz & 1);
  }
  return [];
}

// Whether any page FORM carries annotation (ANTa/ANTz) or text (TXTa/TXTz)
// chunks -- a quick hint for the progress line.
function docFeatures(data: Buffer, offs: number[]): { anno: boolean; text: boolean } {
  let anno = false,
    text = false;
  for (const off of offs) {
    const end = off + 8 + data.readUInt32BE(off + 4);
    let p = off + 12;
    while (p + 8 <= end) {
      const cid = tag(data, p);
      const csz = data.readUInt32BE(p + 4);
      if (cid === "ANTa" || cid === "ANTz") anno = true;
      if (cid === "TXTa" || cid === "TXTz") text = true;
      p += 8 + csz + (csz & 1);
    }
  }
  return { anno, text };
}

// Run a subprocess and capture stdout (async, so files can run concurrently).
async function run(cmd: string[]): Promise<Buffer> {
  const proc = Bun.spawn({ cmd, stdout: "pipe", stderr: "ignore" });
  const out = Buffer.from(await new Response(proc.stdout).arrayBuffer());
  const code = await proc.exited;
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
};

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
): Pick<VerifyResult, "fRender" | "m" | "mm" | "skip" | "bad"> {
  const fRender: number[] = [];
  const bad: string[] = [];
  let m = 0,
    mm = 0,
    skip = 0;

  for (const line of out.toString("latin1").split(/\r?\n/)) {
    if (!line) continue;
    const parts = line.split("\t");
    const kind = parts[0];
    const page = parseInt(parts[1], 10);
    const status = parts[2];
    if (kind === "render") {
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
  return { fRender, m, mm, skip, bad };
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
      pages.map((p) => run([join(RB, "djvutxt.exe"), `--page=${p}`, f])),
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
): Promise<Pick<VerifyResult, "fRender" | "m" | "mm" | "skip" | "bad">> {
  const diffDir = join(VERIFY_DIFFS, safeDirName(name));
  mkdirSync(diffDir, { recursive: true });
  const proc = Bun.spawn({
    cmd: [TEST, "-verify-render", "-diffdir", diffDir, f],
    stdout: "pipe",
    stderr: "ignore",
  });
  const out = Buffer.from(await new Response(proc.stdout).arrayBuffer());
  const code = await proc.exited;
  if (code !== 0 && code !== 1) {
    throw new Error(`${name}: djvu_test -verify-render exited ${code}`);
  }
  return parseVerifyRender(name, out);
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
    stderr: "ignore",
  });
  const out = Buffer.from(await new Response(proc.stdout).arrayBuffer());
  const code = await proc.exited;
  if (code !== 0 && code !== 1) {
    throw new Error(`${name}: djvu_test -verify-text exited ${code}`);
  }
  return parseVerifyText(name, out);
}

async function verifyFile(
  f: string,
  data: Buffer,
  offs: number[],
  hasText: boolean,
): Promise<VerifyResult> {
  const name = basename(f);
  const [render, text] = await Promise.all([
    verifyRender(f, name),
    verifyText(f, offs.length, hasText, name),
  ]);
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
  const SPECS = process.env.DJVU_SPECS ?? join(ROOT, "testfiles");
  const useClang = process.argv.includes("-clang") || defaultUseClang;
  if (process.argv.includes("-clean")) cleanBuildOutput();
  await buildRef();
  TEST = await build(useClang);

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
    const offs = pageOffsets(data);
    const { anno, text: hasText } = docFeatures(data, offs);
    const feats = [`${offs.length} pages`];
    if (anno) feats.push("annots");
    if (hasText) feats.push("text");
    const vr = await verifyFile(f, data, offs, hasText);
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
    const status = diffs.length ? diffs.join(", ") : "same";
    if (diffs.length) {
      failedFiles.set(f, diffs);
      await appendFailure(f, diffs);
    }
    finished++;
    console.log(
      `[${finished}/${files.length}] ${basename(f)} (${feats.join(", ")}) — ` +
        `${humanMs(performance.now() - t0)} — ${status}`,
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