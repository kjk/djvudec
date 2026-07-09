// Compare our subsampled color renders against ddjvu's reduced-size renders.
// Not byte-exact by design (different scaler paths and mask anti-aliasing);
// reports per-page mean/max abs channel diff, flags mean > threshold.
//
//   bun cmd/verify-subsample.ts <file.djvu ... | -rand N> [-sub N] [-pages a,b,c]
//
// Default subsamples: 2 and 3. With no selection it prints usage + the
// available corpus file count.
import { existsSync, mkdirSync, readFileSync } from "fs";
import { join, dirname, basename } from "path";
import { getDeps } from "./get-deps";
import { corpusSummary, selectFiles } from "./corpus";

const ROOT = dirname(import.meta.dir);
const TEST = join(ROOT, "out/msvc/djvu_test_msvc.exe");
const DDJVU = join(ROOT, "ref_build/ddjvu.exe");
const TMP = join(ROOT, "out/verify_sub");
mkdirSync(TMP, { recursive: true });

// mean abs diff threshold (out of 255); anti-aliasing/scaler differences on
// text-heavy compound pages sit well below this
const MEAN_THRESHOLD = 3.0;

interface Pnm {
  w: number;
  h: number;
  comp: number;
  data: Uint8Array;
}

function parsePnm(path: string): Pnm {
  const buf = readFileSync(path);
  let pos = 0;
  function token(): string {
    while (pos < buf.length && /\s/.test(String.fromCharCode(buf[pos]!))) pos++;
    if (buf[pos] === 0x23) {
      // '#' comment
      while (pos < buf.length && buf[pos] !== 0x0a) pos++;
      return token();
    }
    const start = pos;
    while (pos < buf.length && !/\s/.test(String.fromCharCode(buf[pos]!))) pos++;
    return buf.subarray(start, pos).toString();
  }
  const magic = token();
  if (magic !== "P5" && magic !== "P6") throw new Error(`${path}: not P5/P6 (${magic})`);
  const w = parseInt(token());
  const h = parseInt(token());
  const maxv = parseInt(token());
  if (maxv !== 255) throw new Error(`${path}: maxval ${maxv}`);
  pos++; // single whitespace after header
  const comp = magic === "P6" ? 3 : 1;
  const data = new Uint8Array(buf.subarray(pos, pos + w * h * comp));
  if (data.length !== w * h * comp) throw new Error(`${path}: short data`);
  return { w, h, comp, data };
}

async function run(cmd: string[]): Promise<number> {
  const proc = Bun.spawn({ cmd, stdout: "ignore", stderr: "pipe" });
  const err = await new Response(proc.stderr).text();
  const code = await proc.exited;
  if (code !== 0) console.error(`  ${basename(cmd[0]!)} failed: ${err.trim()}`);
  return code;
}

function pageCount(file: string): number {
  const proc = Bun.spawnSync({ cmd: [TEST, "-info", file], stdout: "pipe", stderr: "ignore" });
  const m = proc.stdout.toString().match(/pages:\s*(\d+)/);
  return m ? parseInt(m[1]!) : 0;
}

let failures = 0;
let checked = 0;

async function comparePage(file: string, page: number, sub: number): Promise<void> {
  const ours = join(TMP, "ours.pnm");
  const ref = join(TMP, "ref.pnm");
  if ((await run([TEST, "-page", String(page), "-sub", String(sub), "-out", ours, file])) !== 0) return;
  // -aspect=no: keep the exact ceil(dim/sub) rect (by default the ddjvu CLI
  // shrinks one dimension post-ceil to preserve aspect, a tool-only behavior)
  if ((await run([DDJVU, `-page=${page}`, `-subsample=${sub}`, "-aspect=no", file, ref])) !== 0) return;
  const a = parsePnm(ours);
  const b = parsePnm(ref);
  if (a.w !== b.w || a.h !== b.h) {
    console.log(`  p${page} sub ${sub}: SIZE MISMATCH ours ${a.w}x${a.h} vs ddjvu ${b.w}x${b.h}`);
    failures++;
    return;
  }
  // compare in RGB; expand gray to per-pixel single channel compare
  const n = a.w * a.h;
  let sum = 0;
  let max = 0;
  for (let i = 0; i < n; i++) {
    for (let c = 0; c < 3; c++) {
      const av = a.comp === 3 ? a.data[i * 3 + c]! : a.data[i]!;
      const bv = b.comp === 3 ? b.data[i * 3 + c]! : b.data[i]!;
      const d = Math.abs(av - bv);
      sum += d;
      if (d > max) max = d;
    }
  }
  const mean = sum / (n * 3);
  const ok = mean <= MEAN_THRESHOLD;
  checked++;
  if (!ok) failures++;
  console.log(
    `  p${page} sub ${sub}: ${a.w}x${a.h} ours=${a.comp === 3 ? "ppm" : "pgm"} ref=${b.comp === 3 ? "ppm" : "pgm"} mean ${mean.toFixed(3)} max ${max} ${ok ? "OK" : "FAIL"}`,
  );
}

const args = process.argv.slice(2);
let subs = [2, 3];
let pages: number[] | null = null;
for (let i = 0; i < args.length; i++) {
  if (args[i] === "-sub") subs = [parseInt(args[++i]!)];
  else if (args[i] === "-pages") pages = args[++i]!.split(",").map((s) => parseInt(s));
}
await getDeps();
const files = selectFiles(
  `usage: bun cmd/verify-subsample.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   compare the given files
  -rand N         compare N randomly selected corpus files
options:
  -sub N          check a single subsample (default: 2 and 3)
  -pages a,b,c    check specific pages (default: first, middle, last)

${corpusSummary()}`,
  ["-rand", "-sub", "-pages"],
);

for (const file of files) {
  if (!existsSync(file)) {
    console.error(`missing ${file}`);
    continue;
  }
  const n = pageCount(file);
  const want = pages ?? [1, Math.max(1, Math.ceil(n / 2)), n].filter((p, i, a) => p >= 1 && p <= n && a.indexOf(p) === i);
  console.log(`${basename(file)} (${n} pages)`);
  for (const p of want) for (const s of subs) await comparePage(file, p, s);
}
console.log(`\nverify-subsample: ${checked} renders compared, ${failures} failures (mean threshold ${MEAN_THRESHOLD}/255)`);
process.exit(failures ? 1 : 0);
