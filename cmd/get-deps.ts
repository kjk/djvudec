// get-deps.ts -- fetch the reference checkouts and assemble the test corpus.
//
//   bun cmd/get-deps.ts
//
// Clones the two upstream repos alongside this project (skipped if already
// present), then copies every .djvu sample out of them into testfiles/djvu/.
// Exported as getDeps() so build.ts and tests.ts can ensure deps are in place
// before they build / verify. testfiles/ is gitignored.
import { $ } from "bun";
import { existsSync, mkdirSync, readdirSync, copyFileSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`; // the djvudec project root
const PARENT = join(ROOT, ".."); // siblings (DjvuNet, DjVuLibre) live here
const DEST = join(ROOT, "testfiles", "djvu");

const REPOS = [
  { url: "https://github.com/DjvuNet/DjvuNet", dir: "DjvuNet" },
  { url: "https://github.com/DjvuNet/DjVuLibre", dir: "DjVuLibre" },
];

// Directories (relative to PARENT) to harvest .djvu samples from.
const SAMPLE_DIRS = [
  "DjVuLibre/doc",
  "DjvuNet/Specs",
  "DjvuNet/DjvuNetTest/TestFiles",
];

// Ensure the reference checkouts exist and the test corpus is assembled in
// testfiles/djvu. Returns the corpus directory path.
export async function getDeps(): Promise<string> {
  for (const { url, dir } of REPOS) {
    const path = join(PARENT, dir);
    if (existsSync(path)) {
      console.log(`deps: ${dir} already present`);
      continue;
    }
    console.log(`deps: cloning ${url}`);
    await $`git clone --depth 1 ${url} ${path}`;
  }

  mkdirSync(DEST, { recursive: true });
  let copied = 0;
  for (const rel of SAMPLE_DIRS) {
    const dir = join(PARENT, rel);
    if (!existsSync(dir)) {
      console.log(`deps: sample dir missing, skipping: ${rel}`);
      continue;
    }
    for (const f of readdirSync(dir)) {
      if (!f.toLowerCase().endsWith(".djvu")) continue;
      const dst = join(DEST, f);
      if (existsSync(dst)) continue; // first source wins on name clashes
      copyFileSync(join(dir, f), dst);
      copied++;
    }
  }
  const total = readdirSync(DEST).filter((f) => f.toLowerCase().endsWith(".djvu")).length;
  console.log(`deps: testfiles/djvu ready (${total} files, ${copied} new)`);
  return DEST;
}

if (import.meta.main) await getDeps();
