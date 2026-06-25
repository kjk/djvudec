// copy_djvu_files.ts -- collect .djvu samples from arbitrary directories into
// testfiles/kjk/, skipping content-duplicates.
//
//   bun cmd/copy_djvu_files.ts <dir> [<dir> ...]
//
// Traverses each given directory recursively and copies every .djvu file into
// testfiles/kjk/. Dedup is size-first: at startup we index the name/size of
// every .djvu already under testfiles/ (recursively). A candidate is hashed
// (sha1) only when its size matches an indexed file; SHA-1s are computed lazily
// and cached. Each copied file is added to the index so later candidates dedup
// against it too. Name collisions among distinct files get a numeric suffix.
// Prints the full path and "copied"/"skipped" for every .djvu. testfiles/ is
// gitignored.
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
const TESTFILES = join(ROOT, "testfiles");
const DEST = join(TESTFILES, "kjk");

const dirs = process.argv.slice(2);
if (dirs.length === 0) {
  console.error("usage: bun cmd/copy_djvu_files.ts <dir> [<dir> ...]");
  process.exit(1);
}

const isDjvu = (p: string) => p.toLowerCase().endsWith(".djvu");

function* walkFiles(dir: string): Generator<string> {
  if (!existsSync(dir)) return;
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) yield* walkFiles(p);
    else yield p;
  }
}

// An indexed file: its path plus a lazily-computed, cached sha1.
type Entry = { path: string; sha1?: string };
const sha1Of = (e: Entry): string =>
  (e.sha1 ??= createHash("sha1").update(readFileSync(e.path)).digest("hex"));

// size -> files of that size already known (existing corpus + copies this run).
const bySize = new Map<number, Entry[]>();
function index(path: string, size: number, sha1?: string) {
  const bucket = bySize.get(size);
  const entry: Entry = { path, sha1 };
  if (bucket) bucket.push(entry);
  else bySize.set(size, [entry]);
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

// Seed the size index from every .djvu already under testfiles/ (names + sizes
// only; no hashing yet).
let seeded = 0;
for (const p of walkFiles(TESTFILES)) {
  if (!isDjvu(p)) continue;
  index(p, statSync(p).size);
  seeded++;
}
console.log(`indexed ${seeded} existing .djvu under testfiles/ (by size)`);

let copied = 0,
  skipped = 0;
for (const dir of dirs) {
  if (!existsSync(dir) || !statSync(dir).isDirectory()) {
    console.error(`skip (not a directory): ${dir}`);
    continue;
  }
  for (const p of walkFiles(dir)) {
    if (!isDjvu(p)) continue;
    const size = statSync(p).size;
    const bucket = bySize.get(size);

    let dup = false;
    let sha1: string | undefined;
    if (bucket && bucket.length) {
      // size collision -> compare content
      sha1 = createHash("sha1").update(readFileSync(p)).digest("hex");
      dup = bucket.some((e) => sha1Of(e) === sha1);
    }

    if (dup) {
      skipped++;
      console.log(`${p} skipped`);
    } else {
      const dst = uniqueDest(basename(p));
      copyFileSync(p, dst);
      index(dst, size, sha1); // remember name/size (+sha1 if we computed it)
      copied++;
      console.log(`${p} copied`);
    }
  }
}
console.log(`done: copied ${copied}, skipped ${skipped} -> ${DEST}`);
