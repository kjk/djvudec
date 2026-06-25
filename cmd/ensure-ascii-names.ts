// ensure-ascii-names.ts -- recursively rename files whose names contain
// non-ASCII characters to ASCII-only names, so C code that opens them by path
// (the test harness uses fopen, which is ASCII-only on Windows) can do so.
//
//   bun cmd/ensure-ascii-names.ts <dir>
//
// Walks <dir> recursively. For each file with a non-ASCII name it strips
// diacritics (NFKD) and replaces any remaining non-ASCII character with '_',
// resolving collisions with a numeric suffix. Directories are descended into
// but not renamed. Prints each rename (old -> new) and a progress line every
// 100 files processed.
import { readdirSync, statSync, renameSync, existsSync } from "fs";
import { join, extname } from "path";

const dir = process.argv[2];
if (!dir) {
  console.error("usage: bun cmd/ensure-ascii-names.ts <dir>");
  process.exit(1);
}
if (!existsSync(dir) || !statSync(dir).isDirectory()) {
  console.error(`not a directory: ${dir}`);
  process.exit(1);
}

const hasNonAscii = (s: string) => /[^\x00-\x7F]/.test(s);

// Map a filename to an ASCII-only equivalent: decompose accented letters via
// NFKD and drop the combining marks (é -> e), then replace anything still
// outside ASCII (CJK, Cyrillic, symbols, ...) with '_'.
function toAscii(name: string): string {
  const folded = name.normalize("NFKD").replace(/[̀-ͯ]/g, "");
  let out = "";
  for (const ch of folded) out += ch.codePointAt(0)! <= 0x7f ? ch : "_";
  return out || "_";
}

// An ASCII name for `name` that doesn't already exist in `dir`.
function uniqueAsciiName(dir: string, name: string): string {
  const ascii = toAscii(name);
  if (!existsSync(join(dir, ascii))) return ascii;
  const ext = extname(ascii);
  const stem = ascii.slice(0, ascii.length - ext.length);
  for (let i = 1; ; i++) {
    const cand = `${stem}_${i}${ext}`;
    if (!existsSync(join(dir, cand))) return cand;
  }
}

let processed = 0;

function walk(d: string) {
  for (const entry of readdirSync(d, { withFileTypes: true })) {
    const p = join(d, entry.name);
    if (entry.isDirectory()) {
      walk(p);
      continue;
    }
    processed++;
    if (hasNonAscii(entry.name)) {
      const np = join(d, uniqueAsciiName(d, entry.name));
      process.stdout.write(`rename: ${p} -> ${np}\n`);
      renameSync(p, np);
    }
    if (processed % 100 === 0) process.stdout.write(`processed ${processed} files\n`);
  }
}

walk(dir);
process.stdout.write(`done: processed ${processed} files total\n`);
