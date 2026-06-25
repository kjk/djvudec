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
// ignored). Runs over every .djvu under testfiles/ recursively; set the
// DJVU_SPECS env var to point the scan at a different directory. Files are
// tested in parallel across one worker per CPU; `-cpu N` overrides the count.
import { existsSync, readFileSync, readdirSync, rmSync, statSync } from "fs";
import { tmpdir, cpus } from "os";
import { join, dirname, basename } from "path";
import { getDeps } from "./get-deps";
import { buildRef, build, defaultUseClang } from "./build";

const ROOT = dirname(import.meta.dir);
const RB = join(ROOT, "ref_build");
let TEST = join(ROOT, "djvu_test.exe"); // set to the built exe in main()
const TMP = tmpdir();

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

// 'mask' (pure Sjbz), 'bg' (has an IW44/JPEG background), or 'other'.
function pageKind(data: Buffer, off: number): "mask" | "bg" | "other" {
  const end = off + 8 + data.readUInt32BE(off + 4);
  let p = off + 12;
  let bg = false,
    sj = false;
  while (p + 8 <= end) {
    const cid = tag(data, p);
    const csz = data.readUInt32BE(p + 4);
    if (cid === "BG44" || cid === "FG44" || cid === "BGjp" || cid === "FGjp") bg = true;
    if (cid === "Sjbz") sj = true;
    p += 8 + csz + (csz & 1);
  }
  if (bg) return "bg";
  if (sj) return "mask";
  return "other";
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
  await proc.exited;
  return out;
}
// Run a subprocess that writes to a file; we don't need its stdout.
async function runToFile(cmd: string[]) {
  const proc = Bun.spawn({ cmd, stdout: "ignore", stderr: "ignore" });
  await proc.exited;
}

async function renderRef(f: string, page: number, out: string, fmt = "pgm") {
  await runToFile([join(RB, "ddjvu.exe"), `-format=${fmt}`, `-page=${page}`, f, out]);
}
async function renderMine(f: string, page: number, out: string) {
  if (existsSync(out)) rmSync(out);
  await runToFile([TEST, "-page", String(page), "-out", out, f]);
}

// strip CR / form-feed and trailing whitespace (text normalization)
function textNorm(b: Buffer): string {
  return b
    .toString("latin1")
    .replace(/\r/g, "")
    .replace(/\f/g, "")
    .replace(/\s+$/, "");
}
const getTextRef = (f: string, page: number) =>
  run([join(RB, "djvutxt.exe"), `--page=${page}`, f]);
const getTextMine = (f: string, page: number) =>
  run([TEST, "-page", String(page), "-text", f]);

function readBytes(p: string): Buffer {
  return existsSync(p) ? readFileSync(p) : Buffer.alloc(0);
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

async function main(): Promise<number> {
  // Ensure the corpus + reference checkouts exist, then build everything.
  await getDeps();
  // Verify against every .djvu under testfiles/ (recursively); DJVU_SPECS
  // overrides the root to point at any other directory of samples.
  const SPECS = process.env.DJVU_SPECS ?? join(ROOT, "testfiles");
  // -clang forces the clang harness; default is MSVC on Windows.
  const useClang = process.argv.includes("-clang") || defaultUseClang;
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

  const files = walkDjvu(SPECS).sort();

  // -cpu N overrides the worker count (default: number of logical CPUs).
  const cpuArg = process.argv.indexOf("-cpu");
  const nWorkers = Math.max(
    1,
    Math.min(
      cpuArg >= 0 ? parseInt(process.argv[cpuArg + 1]) || 1 : cpus().length,
      files.length || 1,
    ),
  );
  console.log(`testing ${files.length} files with ${nWorkers} workers`);

  // Test one file: render+text every page, accumulate global counters, and
  // print one line when done. ref/mine are this worker's private temp paths.
  let finished = 0;
  async function testFile(f: string, ref: string, mine: string) {
    const t0 = performance.now();
    const data = readFileSync(f);
    const offs = pageOffsets(data);
    const { anno, text } = docFeatures(data, offs);
    const feats = [`${offs.length} pages`];
    if (anno) feats.push("annots");
    if (text) feats.push("text");
    const fRender: number[] = []; // pages where our render differed
    const fText: number[] = []; // pages where our text differed
    for (let i = 0; i < offs.length; i++) {
      const page = i + 1;
      // render: pure-mask pages -> pgm (gray); bg/color pages -> ppm
      const kind = pageKind(data, offs[i]);
      if (kind === "other") {
        skip++;
      } else {
        const fmt = kind === "mask" ? "pgm" : "ppm";
        await renderRef(f, page, ref, fmt);
        await renderMine(f, page, mine);
        const a = readBytes(ref);
        const b = readBytes(mine);
        if (a.length && a.equals(b)) {
          m++;
        } else {
          mm++;
          fRender.push(page);
          bad.push(`${basename(f)} p${page} ${kind} (ref=${a.length} mine=${b.length})`);
        }
      }
      // text (all pages; ignores trailing page separator)
      const rt = textNorm(await getTextRef(f, page));
      const mt = textNorm(await getTextMine(f, page));
      if (!rt && !mt) te++;
      else if (rt === mt) tm++;
      else {
        tmm++;
        fText.push(page);
        tbad.push(`${basename(f)} p${page}`);
      }
    }
    const diffs: string[] = [];
    if (fRender.length) diffs.push(`render diff p${fRender.join(",p")}`);
    if (fText.length) diffs.push(`text diff p${fText.join(",p")}`);
    const status = diffs.length ? diffs.join(", ") : "same";
    // count this file as finished, then print its line (one atomic line, since
    // workers run concurrently and would otherwise interleave)
    finished++;
    console.log(
      `[${finished}/${files.length}] ${basename(f)} (${feats.join(", ")}) — ` +
        `${humanMs(performance.now() - t0)} — ${status}`,
    );
  }

  // Worker pool: each worker pulls the next file off a shared cursor and runs it
  // with its own temp files so concurrent renders don't clobber each other.
  let next = 0;
  const worker = async (id: number) => {
    const ref = join(TMP, `djref_${id}.pnm`);
    const mine = join(TMP, `djmine_${id}.pnm`);
    for (let idx = next++; idx < files.length; idx = next++) {
      await testFile(files[idx], ref, mine);
    }
  };
  await Promise.all(Array.from({ length: nWorkers }, (_, i) => worker(i)));

  console.log(`render (mask=pgm, bg/color=ppm): MATCH=${m} MISMATCH=${mm}; skipped=${skip}`);
  for (const x of bad.slice(0, 50)) console.log("  render MISMATCH", x);
  console.log(`text: MATCH=${tm} MISMATCH=${tmm}; both-empty=${te}`);
  for (const x of tbad.slice(0, 50)) console.log("  text MISMATCH", x);
  return mm || tmm ? 1 : 0;
}

process.exit(await main());
