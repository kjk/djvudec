// get-deps.ts -- fetch the reference checkouts and assemble the test corpus.
//
//   bun cmd/get-deps.ts
//
// Clones the two upstream repos into deps/ (skipped if already present), then
// copies every .djvu sample out of them into testfiles/djvu/, testfiles/full/,
// and testfiles/subset/ (each destination: skip if that filename is already
// present). Exported as getDeps() so build.ts and tests.ts can ensure deps are
// in place before they build / verify. deps/ and testfiles/ are gitignored.
import { $ } from "bun";
import { existsSync, mkdirSync, readdirSync, copyFileSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`; // the djvudec project root
export const DEPS_DIR = join(ROOT, "deps");
export const DJVULIBRE_DIR = join(DEPS_DIR, "DjVuLibre");
export const DJVUNET_DIR = join(DEPS_DIR, "DjvuNet");
const DEST_DJVU = join(ROOT, "testfiles", "djvu");
const DEST_FULL = join(ROOT, "testfiles", "full");
const DEST_SUBSET = join(ROOT, "testfiles", "subset");

function copyIfAbsent(src: string, name: string, destDir: string): boolean {
  mkdirSync(destDir, { recursive: true });
  const dst = join(destDir, name);
  if (existsSync(dst)) return false;
  copyFileSync(src, dst);
  return true;
}

function countDjvu(dir: string): number {
  if (!existsSync(dir)) return 0;
  return readdirSync(dir).filter((f) => f.toLowerCase().endsWith(".djvu")).length;
}

const REPOS = [
  { url: "https://github.com/DjvuNet/DjvuNet", dir: "DjvuNet" },
  { url: "https://github.com/DjvuNet/DjVuLibre", dir: "DjVuLibre" },
];

// Directories (relative to deps/) to harvest .djvu samples from.
const SAMPLE_DIRS = [
  "DjVuLibre/doc",
  "DjvuNet/Specs",
  "DjvuNet/DjvuNetTest/TestFiles",
];

// Ensure the reference checkouts exist and the test corpus is assembled under
// testfiles/djvu (also mirrored into full/ and subset/). Returns testfiles/djvu.
export async function getDeps(): Promise<string> {
  mkdirSync(DEPS_DIR, { recursive: true });
  for (const { url, dir } of REPOS) {
    const path = join(DEPS_DIR, dir);
    if (existsSync(path)) {
      console.log(`deps: ${dir} already present`);
      continue;
    }
    console.log(`deps: cloning ${url}`);
    await $`git clone --depth 1 ${url} ${path}`;
  }

  const copied = { djvu: 0, full: 0, subset: 0 };
  for (const rel of SAMPLE_DIRS) {
    const dir = join(DEPS_DIR, rel);
    if (!existsSync(dir)) {
      console.log(`deps: sample dir missing, skipping: ${rel}`);
      continue;
    }
    for (const f of readdirSync(dir)) {
      if (!f.toLowerCase().endsWith(".djvu")) continue;
      const src = join(dir, f);
      if (copyIfAbsent(src, f, DEST_DJVU)) copied.djvu++;
      if (copyIfAbsent(src, f, DEST_FULL)) copied.full++;
      if (copyIfAbsent(src, f, DEST_SUBSET)) copied.subset++;
    }
  }
  console.log(
    `deps: testfiles/djvu ready (${countDjvu(DEST_DJVU)} files, ${copied.djvu} new)`,
  );
  console.log(
    `deps: testfiles/full ready (${countDjvu(DEST_FULL)} files, ${copied.full} new)`,
  );
  console.log(
    `deps: testfiles/subset ready (${countDjvu(DEST_SUBSET)} files, ${copied.subset} new)`,
  );
  return DEST_DJVU;
}

if (import.meta.main) await getDeps();
