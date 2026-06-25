// copy_djvu_files.ts -- collect .djvu samples from arbitrary directories into
// testfiles/kjk/, skipping content-duplicates.
//
//   bun cmd/copy_djvu_files.ts <dir> [<dir> ...]
//
// Traverses each given directory recursively and copies every .djvu file into
// testfiles/kjk/. "Non-duplicative" means by *content* (sha256): a file is
// skipped if an identical one already exists in testfiles/djvu, in
// testfiles/kjk, or earlier in this same run. Name collisions among distinct
// files get a numeric suffix. testfiles/ is gitignored.
import {
  readdirSync,
  statSync,
  existsSync,
  mkdirSync,
  readFileSync,
  copyFileSync,
} from "fs";
import { join, basename, extname } from "path";
import { createHash } from "crypto";

const ROOT = `${import.meta.dir}/..`;
const DEST = join(ROOT, "testfiles", "kjk");
// Existing corpora to dedup against (so we never re-copy what we already have).
const SEED_DIRS = [join(ROOT, "testfiles", "djvu"), DEST];

const dirs = process.argv.slice(2);
if (dirs.length === 0) {
  console.error("usage: bun cmd/copy_djvu_files.ts <dir> [<dir> ...]");
  process.exit(1);
}

const isDjvu = (p: string) => p.toLowerCase().endsWith(".djvu");
const hashFile = (p: string) =>
  createHash("sha256").update(readFileSync(p)).digest("hex");

function* walkFiles(dir: string): Generator<string> {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) yield* walkFiles(p);
    else yield p;
  }
}

// A path in DEST for `name` that doesn't already exist (suffix on collision).
function uniqueDest(name: string): string {
  if (!existsSync(join(DEST, name))) return join(DEST, name);
  const ext = extname(name);
  const stem = name.slice(0, name.length - ext.length);
  for (let i = 1; ; i++) {
    const cand = `${stem}_${i}${ext}`;
    if (!existsSync(join(DEST, cand))) return join(DEST, cand);
  }
}

mkdirSync(DEST, { recursive: true });

// Seed the known-content set from the existing corpora.
const seen = new Set<string>();
for (const d of SEED_DIRS) {
  if (!existsSync(d)) continue;
  for (const p of walkFiles(d)) if (isDjvu(p)) seen.add(hashFile(p));
}
console.log(`dedup against ${seen.size} existing .djvu (testfiles/djvu + testfiles/kjk)`);

let scanned = 0,
  copied = 0,
  dupes = 0;
for (const dir of dirs) {
  if (!existsSync(dir) || !statSync(dir).isDirectory()) {
    console.error(`skip (not a directory): ${dir}`);
    continue;
  }
  for (const p of walkFiles(dir)) {
    if (!isDjvu(p)) continue;
    scanned++;
    const h = hashFile(p);
    if (seen.has(h)) {
      dupes++;
    } else {
      seen.add(h);
      const dst = uniqueDest(basename(p));
      copyFileSync(p, dst);
      copied++;
      console.log(`copy: ${p} -> ${dst}`);
    }
    if (scanned % 100 === 0)
      console.log(`scanned ${scanned} .djvu (${copied} copied, ${dupes} dup)`);
  }
}
console.log(`done: scanned ${scanned}, copied ${copied}, skipped ${dupes} duplicates -> ${DEST}`);
