#!/usr/bin/env bun
// verify.ts -- verify djvu_test render output against DjVuLibre's ddjvu, page
// by page (TypeScript port of verify.py; run with `bun cmd/verify.ts`).
//
// Pages that are pure JB2 masks (Sjbz, no BG44/FG44 background) must match
// `ddjvu -format=pgm` byte-for-byte. Pages with an IW44 background or color are
// compared as ppm. Text is compared against djvutxt (trailing separators
// ignored). Override the corpus dir with the DJVU_SPECS env var.
import { existsSync, readFileSync, readdirSync, rmSync } from "fs";
import { tmpdir } from "os";
import { join, dirname, basename } from "path";

const ROOT = dirname(import.meta.dir);
const SPECS = process.env.DJVU_SPECS ?? join(ROOT, "testfiles", "djvunet");
const RB = join(ROOT, "ref_build");
const TEST = join(ROOT, "djvu_test.exe");
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

function run(cmd: string[]): Buffer {
  const r = Bun.spawnSync({ cmd, stdout: "pipe", stderr: "pipe" });
  return Buffer.from(r.stdout);
}

function renderRef(f: string, page: number, out: string, fmt = "pgm") {
  run([join(RB, "ddjvu.exe"), `-format=${fmt}`, `-page=${page}`, f, out]);
}
function renderMine(f: string, page: number, out: string) {
  if (existsSync(out)) rmSync(out);
  run([TEST, "-page", String(page), "-out", out, f]);
}

// strip CR / form-feed and trailing whitespace, like verify.py's text_norm
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

function main(): number {
  let m = 0,
    mm = 0,
    skip = 0;
  let tm = 0,
    tmm = 0,
    te = 0;
  const bad: string[] = [];
  const tbad: string[] = [];

  const files = readdirSync(SPECS)
    .filter((f) => f.toLowerCase().endsWith(".djvu"))
    .sort()
    .map((f) => join(SPECS, f));

  const ref = join(TMP, "djref.pnm");
  const mine = join(TMP, "djmine.pnm");

  for (const f of files) {
    const data = readFileSync(f);
    pageOffsets(data).forEach((o, i) => {
      const page = i + 1;
      // render: pure-mask pages -> pgm (gray); bg/color pages -> ppm
      const kind = pageKind(data, o);
      if (kind === "other") {
        skip++;
      } else {
        const fmt = kind === "mask" ? "pgm" : "ppm";
        renderRef(f, page, ref, fmt);
        renderMine(f, page, mine);
        const a = readBytes(ref);
        const b = readBytes(mine);
        if (a.length && a.equals(b)) {
          m++;
        } else {
          mm++;
          bad.push(`${basename(f)} p${page} ${kind} (ref=${a.length} mine=${b.length})`);
        }
      }
      // text (all pages; ignores trailing page separator)
      const rt = textNorm(getTextRef(f, page));
      const mt = textNorm(getTextMine(f, page));
      if (!rt && !mt) te++;
      else if (rt === mt) tm++;
      else {
        tmm++;
        tbad.push(`${basename(f)} p${page}`);
      }
    });
  }

  console.log(`render (mask=pgm, bg/color=ppm): MATCH=${m} MISMATCH=${mm}; skipped=${skip}`);
  for (const x of bad.slice(0, 50)) console.log("  render MISMATCH", x);
  console.log(`text: MATCH=${tm} MISMATCH=${tmm}; both-empty=${te}`);
  for (const x of tbad.slice(0, 50)) console.log("  text MISMATCH", x);
  return mm || tmm ? 1 : 0;
}

process.exit(main());
