// fuzz.ts -- coverage-guided fuzzing of the djvu decoder (libFuzzer).
//
//   bun cmd/fuzz.ts                 build, seed, fuzz every target round-robin
//                                   on all cores until killed (resumes)
//   bun cmd/fuzz.ts -target jb2,doc only these targets
//   bun cmd/fuzz.ts -repro F        replay one crash artifact and exit
//   bun cmd/fuzz.ts -check-crashes  replay fuzz/crashes/<target>/* (CI)
//   bun cmd/fuzz.ts -minimize       shrink each target's corpus
//
// Targets (test/fuzz_<name>.c, list in FUZZ_TARGETS in build-lib.ts):
//   doc      whole .djvu file -> full public API (container, DIRM, INCL,
//            shared dicts, composition)
//   jb2      Sjbz (+ Djbz dict, + FGbz palette) -> decode + synthesized page
//   iw44     BG44/PM44 chunk sequence -> decode + synthesized page
//   bzz      raw BZZ stream
//   text     TXTa payload (text + zone tree)
//   annot    ANTa S-expressions -> hyperlinks
//   outline  decoded NAVM payload
// A whole multi-MB document runs at < 1 exec/s under ASan; the per-codec
// targets get hundreds to thousands, so most codec bugs are found there. The
// default run gives every target a time slice per round (weighted toward the
// big codecs) with libFuzzer's -fork mode spreading each slice over all cores;
// -ignore_crashes keeps a slice going after a finding, and every new artifact
// is listed at the end.
//
// The fuzzers are always rebuilt from scratch. build-lib.ts does track sources
// and the shared headers, but not the compile FLAGS, and this build's flags
// vary by environment: DJVU_FUZZ_UBSAN switches the sanitizer set
// (address,fuzzer vs address,undefined,fuzzer) and DJVUDEC_FUZZ_CLANG /
// LLVM_PREFIX switch the compiler (and with it the sanitizer runtime). An
// incremental build would reuse objects compiled under the other setting --
// a mixed-instrumentation binary whose failures look like decoder bugs.
//
// fuzz/corpus/<target>/ IS the checkpoint: stop by killing the process, resume
// by running again. An empty corpus is seeded by out/.../fuzz_seeds
// (test/fuzz_seeds.c), which cuts each deps/ corpus file into per-target seeds
// (JB2/IW44 chunks, BZZ streams, decoded text/annotations/outline, standalone
// pages). Findings land in fuzz/crashes/<target>/ (tracked in git as regression
// seeds), reproducible with
//   bun cmd/fuzz.ts -repro fuzz/crashes/<target>/<file>
//
// Windows: libFuzzer + ASan ship with the VS-bundled clang (nothing to install).
// macOS: Apple clang has no fuzzer runtime — needs Homebrew LLVM
// (`brew install llvm`); buildFuzz picks /opt/homebrew/opt/llvm/bin/clang
// (override with DJVUDEC_FUZZ_CLANG).
import {
  existsSync,
  mkdirSync,
  readdirSync,
  copyFileSync,
  renameSync,
  rmSync,
  statSync,
} from "node:fs";
import { availableParallelism } from "node:os";
import { resolve, join, basename, dirname } from "node:path";
import {
  buildFuzz,
  buildLibTool,
  fuzzExe,
  FUZZ_SEEDS_TARGET,
  FUZZ_TARGETS,
  isFuzzTarget,
  type FuzzTarget,
} from "./build-lib";
import { getDeps } from "./get-deps";
import { corpusFiles } from "./corpus";

const ROOT = resolve(import.meta.dir, "..");
const FUZZ = join(ROOT, "fuzz");
const CORPUS = join(FUZZ, "corpus");
const CRASHES = join(FUZZ, "crashes");
const SEEDS = join(ROOT, "out", "fuzz-seeds");

type TargetCfg = {
  maxLen: number; // libFuzzer -max_len
  timeout: number; // -timeout (s): a hang
  slow: number; // -report_slow_units (s)
  weight: number; // relative time slice per round
};

// doc: max_len 300 KB, not the 4 MB it used to be. The doc corpus is
// standalone single pages cut out of the corpus files (fuzz_seeds) plus whole
// files that small; multi-MB inputs ran at < 1 exec/s and libFuzzer replays
// the corpus at the start of every slice.
const CFG: Record<FuzzTarget, TargetCfg> = {
  doc: { maxLen: 300_000, timeout: 20, slow: 10, weight: 2 },
  jb2: { maxLen: 300_000, timeout: 10, slow: 5, weight: 2 },
  iw44: { maxLen: 300_000, timeout: 10, slow: 5, weight: 2 },
  bzz: { maxLen: 100_000, timeout: 10, slow: 5, weight: 1 },
  text: { maxLen: 100_000, timeout: 10, slow: 5, weight: 1 },
  annot: { maxLen: 100_000, timeout: 10, slow: 5, weight: 1 },
  outline: { maxLen: 100_000, timeout: 10, slow: 5, weight: 1 },
};

function usage(): never {
  console.error(
    `usage: bun cmd/fuzz.ts [options]
  (no args)        build, seed empty corpora, fuzz all targets round-robin
                   on all cores until killed (resumes from fuzz/corpus/)
  -target a,b      only these targets (${FUZZ_TARGETS.join(" ")})
  -jobs N          parallel fuzzing processes (default: all ${availableParallelism()} cores)
  -slice SECS      seconds per weight unit per round (default 120)
  -max-len N       override every target's max input size
  -reseed          (re)extract seeds into every selected corpus
  -repro FILE      replay one artifact (target from fuzz/crashes/<target>/)
  -check-crashes   replay every fuzz/crashes/<target>/* artifact (CI)
  -minimize        shrink each selected target's corpus (-merge=1)
  -h, --help`,
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let jobs = availableParallelism();
let slice = 120;
let maxLenOverride = 0;
let repro = "";
let checkCrashes = false;
let minimize = false;
let reseed = false;
let targets: FuzzTarget[] = [...FUZZ_TARGETS];

function intArg(v: string | undefined, name: string): number {
  const n = Number.parseInt(v ?? "", 10);
  if (!Number.isFinite(n) || n < 1) {
    console.error(`${name} requires a positive integer`);
    process.exit(2);
  }
  return n;
}

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "-jobs") jobs = intArg(args[++i], "-jobs");
  else if (a === "-slice") slice = intArg(args[++i], "-slice");
  else if (a === "-max-len") maxLenOverride = intArg(args[++i], "-max-len");
  else if (a === "-repro") repro = args[++i] ?? usage();
  else if (a === "-check-crashes") checkCrashes = true;
  else if (a === "-minimize") minimize = true;
  else if (a === "-reseed") reseed = true;
  else if (a === "-target") {
    const list = (args[++i] ?? usage()).split(",").filter(Boolean);
    for (const t of list) {
      if (!isFuzzTarget(t)) {
        console.error(`unknown target "${t}" (have: ${FUZZ_TARGETS.join(" ")})`);
        process.exit(2);
      }
    }
    targets = list as FuzzTarget[];
  } else if (a === "-h" || a === "--help") usage();
  else usage();
}

const maxLen = (t: FuzzTarget) => maxLenOverride || CFG[t].maxLen;
const corpusDir = (t: FuzzTarget) => join(CORPUS, t);
const crashDir = (t: FuzzTarget) => join(CRASHES, t);

function listArtifacts(t: FuzzTarget): string[] {
  const d = crashDir(t);
  if (!existsSync(d)) return [];
  return readdirSync(d)
    .filter((f) => !f.startsWith("."))
    .map((f) => join(d, f));
}

async function run(exe: string, argv: string[], cwd = FUZZ): Promise<number> {
  const proc = Bun.spawn([exe, ...argv], { cwd, stdout: "inherit", stderr: "inherit" });
  return await proc.exited;
}

// -repro: infer the target from fuzz/crashes/<target>/ (or -target).
if (repro) {
  const path = resolve(repro);
  if (!existsSync(path)) {
    console.error(`no such file: ${path}`);
    process.exit(1);
  }
  const parent = basename(dirname(path));
  const t: FuzzTarget = isFuzzTarget(parent) ? parent : targets.length === 1 ? targets[0] : "doc";
  await buildFuzz(true, [t]);
  console.log(`replaying ${path} on target ${t}`);
  process.exit(await run(fuzzExe(t), [path]));
}

await buildFuzz(true, targets);
for (const t of targets) {
  mkdirSync(corpusDir(t), { recursive: true });
  mkdirSync(crashDir(t), { recursive: true });
}

// -check-crashes: every tracked artifact must complete without ASan/libFuzzer
// crash (exit 0). Used by CI as a fixed regression suite.
if (checkCrashes) {
  let n = 0;
  let fail = 0;
  for (const t of targets) {
    for (const path of listArtifacts(t)) {
      n++;
      const rc = await run(fuzzExe(t), [path]);
      const name = `${t}/${basename(path)}`;
      if (rc !== 0) {
        fail++;
        console.error(`[fail] ${name} exit=${rc}`);
      } else {
        console.log(`[ok] ${name}`);
      }
    }
  }
  if (fail) {
    console.error(`${fail}/${n} artifact(s) still crash`);
    process.exit(1);
  }
  console.log(`check-crashes: ${n} ok`);
  process.exit(0);
}

// -minimize: merge each corpus into a fresh minimal covering set, then swap.
if (minimize) {
  for (const t of targets) {
    const dir = corpusDir(t);
    const tmp = `${dir}.min`;
    rmSync(tmp, { recursive: true, force: true });
    mkdirSync(tmp, { recursive: true });
    console.log(`minimizing ${t} corpus (libFuzzer -merge=1)...`);
    const rc = await run(fuzzExe(t), ["-merge=1", `-max_len=${maxLen(t)}`, tmp, dir]);
    if (rc !== 0) {
      console.error(`merge failed (${rc}); leaving ${t} corpus untouched`);
      rmSync(tmp, { recursive: true, force: true });
      continue;
    }
    const before = readdirSync(dir).length;
    rmSync(dir, { recursive: true, force: true });
    renameSync(tmp, dir);
    console.log(`${t}: corpus minimized ${before} -> ${readdirSync(dir).length} inputs`);
  }
  process.exit(0);
}

// A pre-multi-target checkout kept one flat corpus of whole documents in
// fuzz/corpus/; park it out of the way (it is regenerable, and mostly multi-MB
// inputs the doc target now skips).
const flat = readdirSync(CORPUS).filter((f) => statSync(join(CORPUS, f)).isFile());
if (flat.length > 0) {
  const old = join(FUZZ, "corpus.old");
  mkdirSync(old, { recursive: true });
  for (const f of flat) renameSync(join(CORPUS, f), join(old, f));
  console.log(`moved ${flat.length} legacy flat corpus file(s) to ${old} (safe to delete)`);
}

// Seed empty corpora (or all selected ones with -reseed).
const needSeed = targets.filter((t) => reseed || readdirSync(corpusDir(t)).length === 0);
if (needSeed.length > 0) {
  await getDeps();
  const files = corpusFiles();
  const seedExe = await buildLibTool(FUZZ_SEEDS_TARGET);
  rmSync(SEEDS, { recursive: true, force: true });
  for (const t of FUZZ_TARGETS) mkdirSync(join(SEEDS, t), { recursive: true });
  const rc = await run(seedExe, [SEEDS, ...files], ROOT);
  if (rc !== 0) {
    console.error(`fuzz_seeds failed (exit ${rc})`);
    process.exit(1);
  }
  for (const t of needSeed) {
    let n = 0;
    const src = join(SEEDS, t);
    for (const f of readdirSync(src)) {
      if (statSync(join(src, f)).size > maxLen(t)) continue;
      copyFileSync(join(src, f), join(corpusDir(t), f));
      n++;
    }
    if (t === "doc") {
      // whole documents too (multi-page structure, DIRM, INCL, shared dicts)
      for (const f of files) {
        if (statSync(f).size > maxLen(t)) continue;
        copyFileSync(f, join(corpusDir(t), basename(f)));
        n++;
      }
    }
    console.log(`seeded ${t} corpus with ${n} input(s)`);
  }
}

const before = new Set(targets.flatMap(listArtifacts));

// Swallow SIGINT in the parent so that when you Ctrl-C, the child (which shares
// the console and receives the signal too) exits first and we still print the
// crash summary below.
let interrupted = false;
process.on("SIGINT", () => {
  interrupted = true;
});

function fuzzArgs(t: FuzzTarget, secs: number): string[] {
  const c = CFG[t];
  return [
    corpusDir(t),
    `-artifact_prefix=${crashDir(t)}/`,
    `-max_len=${maxLen(t)}`,
    "-rss_limit_mb=4096",
    `-timeout=${c.timeout}`,
    `-report_slow_units=${c.slow}`,
    `-max_total_time=${secs}`,
    "-print_final_stats=1",
    // -fork: N child processes, corpus merged back into corpusDir after each
    // job; keep going past findings (each still lands in crashDir).
    ...(jobs > 1 ? [`-fork=${jobs}`, "-ignore_crashes=1", "-ignore_timeouts=1", "-ignore_ooms=1"] : []),
  ];
}

const one = targets.length === 1;
console.log(
  `fuzzing [${targets.join(" ")}] with ${jobs} process(es)` +
    (one ? "" : `, ${slice}s x weight per target per round`) +
    `; corpus=${CORPUS}\n  Ctrl-C to stop; rerun to resume.`,
);

let rc = 0;
let round = 0;
while (!interrupted) {
  round++;
  for (const t of targets) {
    if (interrupted) break;
    // A single target just runs until killed (one very long slice).
    const secs = one ? 365 * 24 * 3600 : slice * CFG[t].weight;
    console.log(`\n=== round ${round}: ${t} for ${secs}s ===`);
    rc = await run(fuzzExe(t), fuzzArgs(t, secs));
    // With jobs == 1 libFuzzer stops at the first finding: move on to the
    // next target (the artifact is listed at the end) instead of quitting.
  }
  if (one) break;
}

const fresh = targets.flatMap(listArtifacts).filter((f) => !before.has(f));
if (fresh.length > 0) {
  console.log(`\n${fresh.length} new crash artifact(s):`);
  for (const f of fresh) {
    console.log(`  ${f}`);
    console.log(`    reproduce: bun cmd/fuzz.ts -repro ${f}`);
  }
  process.exit(1);
}
process.exit(interrupted ? 0 : rc);
