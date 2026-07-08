// fuzz.ts -- coverage-guided fuzzing of the djvu decoder (libFuzzer).
//
//   bun cmd/fuzz.ts             build, seed corpus if empty, fuzz until killed
//   bun cmd/fuzz.ts -jobs 8     run 8 parallel workers sharing the corpus
//   bun cmd/fuzz.ts -repro F    replay a single crash artifact and exit
//   bun cmd/fuzz.ts -minimize   shrink the corpus to a minimal covering set
//
// The fuzzer is always rebuilt from scratch: build-lib.ts doesn't track header
// dependencies, so an edit to djvu_internal.h could otherwise leave mixed
// object files (some against the old struct layout) -- an ABI mismatch that
// looks like a decoder crash. A clean build is cheap here and removes that trap.
//
// The corpus directory (fuzz/corpus) IS the checkpoint: stop by killing the
// process, resume by running again -- libFuzzer reloads the corpus and keeps
// going. Crashes land in fuzz/corpus's sibling fuzz/crashes (tracked in git as
// regression seeds); each is a .djvu-shaped input reproducible with
//   out/fuzz/djvudec_fuzz.exe <crashfile>
//
// No install needed: libFuzzer + ASan ship with the VS-bundled clang.
import {
  existsSync,
  mkdirSync,
  readdirSync,
  copyFileSync,
  renameSync,
  rmSync,
} from "node:fs";
import { resolve, join } from "node:path";
import { buildFuzz, FUZZ_EXE } from "./build-lib";

const ROOT = resolve(import.meta.dir, "..");
const FUZZ = join(ROOT, "fuzz");
const CORPUS = join(FUZZ, "corpus");
const CRASHES = join(FUZZ, "crashes");
const SEED_DIR = join(ROOT, "testfiles", "subset");

function usage(): never {
  console.error(
    `usage: bun cmd/fuzz.ts [options]
  (no args)      build, seed corpus if empty, fuzz until killed (resumes)
  -jobs N        run N parallel workers sharing the corpus
  -max-len N     max input size in bytes (default 4000000)
  -repro FILE    replay a single crash artifact and exit
  -minimize      shrink the corpus to a minimal covering set
  -h, --help`,
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let jobs = 1;
let maxLen = 4000000;
let repro = "";
let minimize = false;

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "-jobs") jobs = intArg(args[++i], "-jobs");
  else if (a === "-max-len") maxLen = intArg(args[++i], "-max-len");
  else if (a === "-repro") repro = args[++i] ?? usage();
  else if (a === "-minimize") minimize = true;
  else if (a === "-h" || a === "--help") usage();
  else usage();
}

function intArg(v: string | undefined, name: string): number {
  const n = Number.parseInt(v ?? "", 10);
  if (!Number.isFinite(n) || n < 1) {
    console.error(`${name} requires a positive integer`);
    process.exit(2);
  }
  return n;
}

// Artifact kinds libFuzzer writes on a finding (all reproducible by replay).
const isArtifact = (name: string) =>
  /^(crash|timeout|oom|leak)-/.test(name);

function listArtifacts(): string[] {
  if (!existsSync(CRASHES)) return [];
  return readdirSync(CRASHES).filter(isArtifact);
}

async function run(argv: string[]): Promise<number> {
  const proc = Bun.spawn([FUZZ_EXE, ...argv], {
    cwd: FUZZ,
    stdout: "inherit",
    stderr: "inherit",
  });
  return await proc.exited;
}

const exe = await buildFuzz(true); // always clean-build (see header comment)

mkdirSync(CORPUS, { recursive: true });
mkdirSync(CRASHES, { recursive: true });

// -repro: replay one artifact with a full stack trace, then exit.
if (repro) {
  const path = resolve(repro);
  if (!existsSync(path)) {
    console.error(`no such file: ${path}`);
    process.exit(1);
  }
  console.log(`replaying ${path}`);
  process.exit(await run([path]));
}

// -minimize: merge the corpus into a fresh minimal covering set, then swap.
if (minimize) {
  const tmp = join(FUZZ, "corpus.min");
  rmSync(tmp, { recursive: true, force: true });
  mkdirSync(tmp, { recursive: true });
  console.log("minimizing corpus (libFuzzer -merge=1)...");
  const rc = await run(["-merge=1", "corpus.min", "corpus"]);
  if (rc !== 0) {
    console.error(`merge failed (${rc}); leaving corpus untouched`);
    rmSync(tmp, { recursive: true, force: true });
    process.exit(rc);
  }
  const before = readdirSync(CORPUS).length;
  rmSync(CORPUS, { recursive: true, force: true });
  renameSync(tmp, CORPUS);
  const after = readdirSync(CORPUS).length;
  console.log(`corpus minimized: ${before} -> ${after} inputs`);
  process.exit(0);
}

// First run: seed the (empty) corpus with the real .djvu files from the
// verify subset so mutation starts from valid DjVu, not random bytes.
if (readdirSync(CORPUS).length === 0 && existsSync(SEED_DIR)) {
  const seeds = readdirSync(SEED_DIR).filter((f) => /\.djvu?$/i.test(f));
  for (const f of seeds) copyFileSync(join(SEED_DIR, f), join(CORPUS, f));
  console.log(`seeded corpus with ${seeds.length} file(s) from testfiles/subset`);
}

const before = new Set(listArtifacts());

// Swallow SIGINT in the parent so that when you Ctrl-C, the child (which shares
// the console and receives the signal too) exits first and we still print the
// crash summary below.
process.on("SIGINT", () => {});

const fuzzArgs = [
  "corpus",
  "-artifact_prefix=crashes/",
  `-max_len=${maxLen}`,
  "-rss_limit_mb=4096",
  "-timeout=25",
  // The biggest seed (2.9MB book) honestly costs ~5s under ASan; libFuzzer's
  // default 10s slow-unit reporter flags its mutations under fuzzing load.
  "-report_slow_units=20",
  "-print_final_stats=1",
  ...(jobs > 1 ? [`-jobs=${jobs}`, `-workers=${jobs}`] : []),
];

console.log(
  `fuzzing (${jobs} ${jobs === 1 ? "process" : "workers"}); ` +
    `corpus=${CORPUS}\n  Ctrl-C to stop; rerun to resume.`,
);
const rc = await run(fuzzArgs);

const fresh = listArtifacts().filter((f) => !before.has(f));
if (fresh.length > 0) {
  console.log(`\n${fresh.length} new crash artifact(s) in fuzz/crashes:`);
  for (const f of fresh) {
    console.log(`  ${f}`);
    console.log(`    reproduce: ${exe} ${join(CRASHES, f)}`);
    console.log(`    or:        bun cmd/fuzz.ts -repro ${join("fuzz", "crashes", f)}`);
  }
  process.exit(1);
}
process.exit(rc);
