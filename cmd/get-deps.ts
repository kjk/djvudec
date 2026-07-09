// get-deps.ts -- fetch the reference checkouts (which double as the corpus).
//
//   bun cmd/get-deps.ts
//
// Clones the upstream repos into deps/ (skipped if already present). The
// .djvu samples inside these checkouts ARE the test/bench corpus -- there is
// no separate testfiles/ mirror; cmd/corpus.ts enumerates them in place.
// Exported as getDeps() so build.ts and tests.ts can ensure deps are in
// place before they build / verify. deps/ is gitignored.
import { $ } from "bun";
import { existsSync, mkdirSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`; // the djvudec project root
export const DEPS_DIR = join(ROOT, "deps");
export const DJVULIBRE_DIR = join(DEPS_DIR, "DjVuLibre");
export const DJVUNET_DIR = join(DEPS_DIR, "DjvuNet");

const REPOS = [
  { url: "https://github.com/DjvuNet/DjvuNet", dir: "DjvuNet" },
  { url: "https://github.com/DjvuNet/DjVuLibre", dir: "DjVuLibre" },
];

// DjvuNet's test-artifacts repo: 98 .djvu files (~95 MB), the corpus
// DjvuNet.Tests runs against. The repo also carries hundreds of MB of
// unrelated blobs (PNG/JSON/bin), so it is cloned blobless + sparse with
// only the .djvu files materialized.
const ARTIFACTS = {
  url: "https://github.com/DjvuNet/artifacts",
  dir: "artifacts",
};

// Ensure the reference checkouts (and with them the corpus) exist.
export async function getDeps(): Promise<void> {
  mkdirSync(DEPS_DIR, { recursive: true });
  for (const { url, dir } of REPOS) {
    const path = join(DEPS_DIR, dir);
    if (existsSync(path)) continue;
    console.log(`deps: cloning ${url}`);
    await $`git clone --depth 1 ${url} ${path}`;
  }

  const path = join(DEPS_DIR, ARTIFACTS.dir);
  if (!existsSync(path)) {
    console.log(`deps: cloning ${ARTIFACTS.url} (sparse, .djvu only)`);
    /* --no-checkout so the sparse pattern is in place before any file
       materializes; otherwise the initial checkout writes the root files
       and leaves them dirty after the pattern switch. */
    await $`git clone --depth 1 --filter=blob:none --no-checkout ${ARTIFACTS.url} ${path}`;
    await $`git -C ${path} sparse-checkout set --no-cone ${"*.djvu"}`;
    await $`git -C ${path} checkout`;
  }
}

if (import.meta.main) {
  await getDeps();
  const { corpusSummary } = await import("./corpus");
  console.log(`deps: ready; ${corpusSummary()}`);
}
