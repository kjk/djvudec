// Test/bench corpus: the .djvu files inside the reference checkouts under
// deps/ (cloned by get-deps.ts). There is no local testfiles/ mirror and no
// full/subset split; scripts take explicit files, or -rand N to sample the
// corpus. DJVU_SPECS overrides the corpus with any directory of .djvu files
// (scanned recursively).
import { existsSync, readdirSync, statSync } from "fs";
import { basename, isAbsolute, join, relative } from "path";
import { DEPS_DIR } from "./get-deps";

// Directories (relative to deps/) that hold the corpus .djvu files.
const CORPUS_DIRS = [
  "DjVuLibre/doc",
  "DjvuNet/Specs",
  "DjvuNet/DjvuNetTest/TestFiles",
  "artifacts",
  "artifacts/data",
  "artifacts/wikimedia",
];

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

// Every corpus .djvu (sorted, deduped by basename -- the same sample can
// appear in more than one reference checkout). DJVU_SPECS overrides.
export function corpusFiles(): string[] {
  if (process.env.DJVU_SPECS) return walkDjvu(process.env.DJVU_SPECS).sort();
  const seen = new Set<string>();
  const out: string[] = [];
  for (const rel of CORPUS_DIRS) {
    for (const f of readdirSync2(join(DEPS_DIR, rel))) {
      const name = basename(f).toLowerCase();
      if (seen.has(name)) continue;
      seen.add(name);
      out.push(f);
    }
  }
  return out.sort();
}

// non-recursive .djvu listing (CORPUS_DIRS name each directory explicitly)
function readdirSync2(dir: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.toLowerCase().endsWith(".djvu"))
    .map((f) => join(dir, f))
    .filter((p) => !statSync(p).isDirectory());
}

// N random corpus files (all of them if N >= count).
export function pickRandom(files: string[], n: number): string[] {
  const shuffled = [...files];
  for (let i = shuffled.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
  }
  return shuffled.slice(0, Math.max(0, Math.min(n, shuffled.length)));
}

// Shared file-selection for scripts that operate on .djvu files: explicit
// paths, -rand N random corpus files, or -all for every corpus file. With
// none of those, prints usageText (which should include corpusSummary())
// and exits 2. valueFlags lists the script's flags that take a value, so
// those values aren't taken for paths.
export function selectFiles(
  usageText: string,
  valueFlags: string[] = ["-rand"],
): string[] {
  const argv = process.argv.slice(2);
  const explicit = argv.filter(
    (a, i) => !a.startsWith("-") && !valueFlags.includes(argv[i - 1] ?? ""),
  );
  if (argv.includes("-all")) return corpusFiles();
  const ri = argv.indexOf("-rand");
  if (ri >= 0) {
    const n = parseInt(argv[ri + 1] ?? "");
    if (!(n > 0)) {
      console.log(usageText);
      process.exit(2);
    }
    const all = corpusFiles();
    const picked = pickRandom(all, n);
    console.log(`(${picked.length} random of ${all.length} corpus files)`);
    return picked;
  }
  if (explicit.length > 0) {
    for (const f of explicit) {
      if (!existsSync(f)) {
        console.error(`no such file: ${f}`);
        process.exit(1);
      }
    }
    return explicit;
  }
  console.log(usageText);
  process.exit(2);
}

// "1,234,567" -- exact byte count with thousands separators.
export function fmtBytesExact(n: number): string {
  return n.toLocaleString("en-US");
}

// "1.2 MB" -- human-readable size.
export function fmtBytesHuman(n: number): string {
  if (n >= 1024 ** 3) return `${(n / 1024 ** 3).toFixed(2)} GB`;
  if (n >= 1024 ** 2) return `${(n / 1024 ** 2).toFixed(1)} MB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${n} B`;
}

// "path/to/file.djvu (1.2 MB, 1,234,567 bytes)" with the path relative to
// the repo root when it is inside it.
export function fileLabel(f: string, root: string): string {
  let rel = relative(root, f);
  if (rel.startsWith("..") || isAbsolute(rel)) rel = f;
  rel = rel.replaceAll("\\", "/");
  const size = statSync(f).size;
  return `${rel} (${fmtBytesHuman(size)}, ${fmtBytesExact(size)} bytes)`;
}

// One-line corpus summary for usage screens.
export function corpusSummary(): string {
  const n = corpusFiles().length;
  const src = process.env.DJVU_SPECS
    ? `DJVU_SPECS=${process.env.DJVU_SPECS}`
    : "deps/ reference checkouts (run `bun cmd/get-deps.ts` to fetch)";
  return `${n} .djvu file(s) available from ${src}`;
}
