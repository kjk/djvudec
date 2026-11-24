// fuzz.ts -- coverage-guided fuzzing of the djvu decoder (libFuzzer).
//
//   bun cmd/fuzz.ts                 build, seed, fuzz every target round-robin
//                                   on all cores until killed (resumes)
//   bun cmd/fuzz.ts -target jb2,doc only these targets
//   bun cmd/fuzz.ts -repro F        replay one crash artifact and exit
//   bun cmd/fuzz.ts -check-crashes  replay fuzz/crashes/<target>/* (CI)
//   bun cmd/fuzz.ts -minimize       shrink each target's corpus
//   bun cmd/fuzz.ts -coverage       source coverage of src/ from the corpora
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
//   diff     whole .djvu, rendered by us AND DjVuLibre (linked libdjvu): any
//            output difference beyond the palette-overlap rule aborts, so
//            this finds wrong output, not just memory errors. Not in the
//            default set on Windows (libdjvu + libFuzzer link untested there;
//            pass -target diff to try).
// A whole multi-MB document runs at < 1 exec/s under ASan; the per-codec
// targets get hundreds to thousands, so most codec bugs are found there. The
// default run gives every target a time slice per round (weighted toward the
// big codecs) with libFuzzer's -fork mode spreading each slice over all cores;
// -ignore_crashes keeps a slice going after a finding, and every new artifact
// is listed at the end.
//
// Sanitizers: the default build is ASan + UBSan (non-recoverable, so a UB
// finding aborts and is saved; Linux/macOS -- Windows is ASan only). On Linux
// the default run alternates rounds between that build and a MemorySanitizer
// build (out/fuzz_msan/, uninitialized reads ASan cannot see; the diff target
// is skipped there). -msan / -asan pin one; -msan also applies to -repro,
// -check-crashes and -minimize.
//
// AFL++: when afl-fuzz + afl-clang-fast are installed (Linux/macOS), the
// default rotation adds an AFL++ round (asan, msan, afl, ...): the same
// harnesses built with afl-clang-fast (ASan+UBSan, persistent mode via
// libAFLDriver) plus a CmpLog companion (-c), one main + secondary instances
// over -jobs cores, the libFuzzer dictionaries (-x). The two engines share
// finds both ways: afl-fuzz imports fuzz/corpus/<t> (-F) and libFuzzer reads
// fuzz/afl/<t>/*/queue as extra corpus dirs. AFL crashes are copied into
// fuzz/crashes/<t>/ (afl-<hash>) so -repro / -check-crashes replay them on
// the libFuzzer build. -afl runs only AFL++; -no-afl leaves it out.
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
  readFileSync,
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
  llvmTool,
  msanSupported,
  aflAvailable,
  type FuzzSan,
  type FuzzTarget,
} from "./build-lib";
import { getDeps } from "./get-deps";
import { corpusFiles } from "./corpus";
import { isWindows } from "./util";

const ROOT = resolve(import.meta.dir, "..");
const FUZZ = join(ROOT, "fuzz");
const CORPUS = join(FUZZ, "corpus");
const CRASHES = join(FUZZ, "crashes");
const SEEDS = join(ROOT, "out", "fuzz-seeds");
// fuzz/dict/<target>.dict (libFuzzer -dict): chunk ids, FORM types, field
// encodings and annotation keywords the byte mutators would never guess.
const DICTS = join(FUZZ, "dict");
const dictArg = (t: FuzzTarget) => {
  const f = join(DICTS, `${inputKind(t)}.dict`);
  return existsSync(f) ? [`-dict=${f}`] : [];
};

type TargetCfg = {
  maxLen: number; // libFuzzer -max_len
  extra?: string[]; // more libFuzzer flags
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
  // DjVuLibre leaks on some error paths: LeakSanitizer would report the
  // oracle's leaks as findings (ours are still caught by the budgeted
  // allocator's per-input check).
  diff: { maxLen: 300_000, timeout: 30, slow: 15, weight: 2, extra: ["-detect_leaks=0"] },
};

// Inputs, seeds and dictionary are shared: diff takes whole documents like doc.
const inputKind = (t: FuzzTarget): FuzzTarget => (t === "diff" ? "doc" : t);

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
  -asan / -msan    only the ASan+UBSan / MemorySanitizer build (default on
                   Linux: alternate rounds; elsewhere ASan only)
  -afl / -no-afl   only AFL++ / no AFL++ rounds (default: AFL++ rounds join
                   the rotation when afl-fuzz is installed)
  -repro FILE      replay one artifact (target from fuzz/crashes/<target>/)
  -check-crashes   replay every fuzz/crashes/<target>/* artifact (CI)
  -minimize        shrink each selected target's corpus (-merge=1)
  -coverage        replay the corpora on coverage builds; print per-file
                   coverage of src/ and write an HTML report
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
let coverage = false;
let reseed = false;
let sanMode: FuzzSan | "auto" = "auto";
let noAfl = false;
let targets: FuzzTarget[] = FUZZ_TARGETS.filter((t) => !(isWindows && t === "diff"));

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
  else if (a === "-coverage") coverage = true;
  else if (a === "-reseed") reseed = true;
  else if (a === "-asan") sanMode = "asan";
  else if (a === "-msan") sanMode = "msan";
  else if (a === "-afl") sanMode = "afl";
  else if (a === "-no-afl") noAfl = true;
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

if (sanMode === "msan" && !msanSupported) {
  console.error("-msan needs Linux (clang has no MemorySanitizer on Windows/macOS)");
  process.exit(2);
}
const oneSan: FuzzSan = sanMode === "msan" ? "msan" : "asan";

// -repro: infer the target from fuzz/crashes/<target>/ (or -target).
if (repro) {
  const path = resolve(repro);
  if (!existsSync(path)) {
    console.error(`no such file: ${path}`);
    process.exit(1);
  }
  const parent = basename(dirname(path));
  const t: FuzzTarget = isFuzzTarget(parent) ? parent : targets.length === 1 ? targets[0] : "doc";
  await buildFuzz(true, [t], oneSan);
  console.log(`replaying ${path} on target ${t} (${oneSan})`);
  process.exit(await run(fuzzExe(t, oneSan), [...(CFG[t].extra ?? []), path]));
}

// -coverage: replay each corpus on a coverage build (no sanitizer), merge the
// profiles, report per-file coverage of src/ -- where fuzzing never reaches
// is where new seeds (or a new target) would help.
if (coverage) {
  const profdata = llvmTool("llvm-profdata");
  const cov = llvmTool("llvm-cov");
  if (!profdata || !cov) {
    console.error("llvm-profdata / llvm-cov not found (next to the fuzz clang, on PATH, or -<version>)");
    process.exit(2);
  }
  const out = join(ROOT, "out", "fuzz_cov");
  const prof = join(out, "prof");
  await buildFuzz(true, targets, "cov");
  rmSync(prof, { recursive: true, force: true });
  mkdirSync(prof, { recursive: true });
  const exes: string[] = [];
  for (const t of targets) {
    const dir = corpusDir(t);
    if (!existsSync(dir) || readdirSync(dir).length === 0) {
      console.log(`${t}: empty corpus, skipped (run fuzz.ts once to seed it)`);
      continue;
    }
    console.log(`${t}: replaying ${readdirSync(dir).length} input(s)...`);
    // %p: one profile per process (libFuzzer may fork); timeouts generous --
    // a coverage build is uninstrumented by sanitizers but still unoptimized.
    const proc = Bun.spawn(
      [fuzzExe(t, "cov"), "-runs=0", `-max_len=${maxLen(t)}`, "-rss_limit_mb=4096", "-timeout=60", ...(CFG[t].extra ?? []), dir],
      { env: { ...process.env, LLVM_PROFILE_FILE: join(prof, `${t}-%p.profraw`) }, stdout: "ignore", stderr: "ignore" },
    );
    await proc.exited;
    exes.push(fuzzExe(t, "cov"));
  }
  const raws = readdirSync(prof).filter((f) => f.endsWith(".profraw")).map((f) => join(prof, f));
  if (raws.length === 0) {
    console.error("no profiles written");
    process.exit(1);
  }
  const merged = join(out, "merged.profdata");
  let p = Bun.spawnSync([profdata, "merge", "-sparse", "-o", merged, ...raws], { stderr: "inherit" });
  if (p.exitCode !== 0) process.exit(1);
  const objArgs = exes.slice(1).flatMap((e) => ["-object", e]);
  const src = join(ROOT, "src");
  p = Bun.spawnSync([cov, "report", exes[0], ...objArgs, `-instr-profile=${merged}`, "-ignore-filename-regex=debug\\.c$", src], { stderr: "inherit" });
  process.stdout.write(p.stdout.toString());
  const html = join(out, "html");
  rmSync(html, { recursive: true, force: true });
  Bun.spawnSync([cov, "show", exes[0], ...objArgs, `-instr-profile=${merged}`, "-format=html", `-output-dir=${html}`, "-ignore-filename-regex=debug\\.c$", src], { stderr: "inherit" });
  console.log(`\nHTML report: ${join(html, "index.html")}`);
  process.exit(0);
}

// Fuzzing alternates sanitizer builds by round; every other mode uses one.
if (sanMode === "afl" && !aflAvailable()) {
  console.error("-afl: AFL++ not found (afl-fuzz / afl-clang-fast on PATH; e.g. apt install afl++)");
  process.exit(2);
}
const fuzzSans: FuzzSan[] =
  sanMode !== "auto"
    ? [sanMode]
    : [
        "asan",
        ...(msanSupported ? (["msan"] as FuzzSan[]) : []),
        ...(aflAvailable() && !noAfl ? (["afl"] as FuzzSan[]) : []),
      ];
const modeOnly = checkCrashes || minimize;
for (const s of modeOnly ? [oneSan] : fuzzSans) {
  await buildFuzz(true, targets, s);
  if (s === "afl") await buildFuzz(true, targets, "aflcmp");
}
// the diff target links the uninstrumented libdjvu oracle: no MSan or AFL++
// build of it
const hasExe = (t: FuzzTarget, s: FuzzSan) => !((s === "msan" || s === "afl") && t === "diff");
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
    if (!hasExe(t, oneSan)) continue;
    for (const path of listArtifacts(t)) {
      n++;
      const rc = await run(fuzzExe(t, oneSan), [...(CFG[t].extra ?? []), path]);
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
    if (!hasExe(t, oneSan)) continue;
    const rc = await run(fuzzExe(t, oneSan), ["-merge=1", `-max_len=${maxLen(t)}`, tmp, dir]);
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
    const src = join(SEEDS, inputKind(t));
    for (const f of readdirSync(src)) {
      if (statSync(join(src, f)).size > maxLen(t)) continue;
      copyFileSync(join(src, f), join(corpusDir(t), f));
      n++;
    }
    if (inputKind(t) === "doc") {
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

// AFL++ state per target: fuzz/afl/<t>/<instance>/{queue,crashes,...}.
const aflDir = (t: FuzzTarget) => join(FUZZ, "afl", t);
const aflQueues = (t: FuzzTarget): string[] => {
  const d = aflDir(t);
  if (!existsSync(d)) return [];
  return readdirSync(d)
    .map((i) => join(d, i, "queue"))
    .filter((q) => existsSync(q));
};

// Copy AFL++ crashes into fuzz/crashes/<t>/ as afl-<content hash> (dedupes
// across rounds; replayable with -repro on the libFuzzer build).
function collectAflCrashes(t: FuzzTarget): void {
  const d = aflDir(t);
  if (!existsSync(d)) return;
  for (const inst of readdirSync(d)) {
    const cd = join(d, inst, "crashes");
    if (!existsSync(cd)) continue;
    for (const f of readdirSync(cd)) {
      if (!f.startsWith("id:")) continue;
      const data = readFileSync(join(cd, f));
      const h = new Bun.CryptoHasher("sha1").update(data).digest("hex").slice(0, 16);
      const dst = join(crashDir(t), `afl-${h}`);
      if (!existsSync(dst)) copyFileSync(join(cd, f), dst);
    }
  }
}

// One AFL++ round for target t: main (-M) + secondaries (-S) over -jobs
// cores, each stopping after secs (-V). A secondary that fails
// to start is reported (its output is otherwise discarded).
async function runAfl(t: FuzzTarget, secs: number): Promise<number> {
  const out = aflDir(t);
  mkdirSync(out, { recursive: true });
  const dict = join(DICTS, `${inputKind(t)}.dict`);
  const env = {
    ...process.env,
    AFL_NO_UI: "1",
    AFL_AUTORESUME: "1",
    AFL_SKIP_CPUFREQ: "1",
    AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES: "1",
    AFL_IMPORT_FIRST: "1",
  };
  const procs = [];
  for (let i = 0; i < jobs; i++) {
    const args = [
      "-i", corpusDir(t), "-o", out,
      i === 0 ? "-M" : "-S", i === 0 ? "main" : `s${i}`,
      // -F (import libFuzzer's corpus) is main-only; the secondaries get
      // it through the main's queue. CmpLog on the main only too: AFL++
      // recommends one or two CmpLog instances, not all.
      ...(i === 0 ? ["-F", corpusDir(t), "-c", fuzzExe(t, "aflcmp")] : []),
      "-V", String(secs),
      "-t", `${CFG[t].timeout * 1000}+`,
      "-m", "none",
      ...(existsSync(dict) ? ["-x", dict] : []),
      "--", fuzzExe(t, "afl"),
    ];
    procs.push(
      Bun.spawn(["afl-fuzz", ...args], {
        env,
        stdout: i === 0 ? "inherit" : "ignore",
        stderr: i === 0 ? "inherit" : "ignore",
      }),
    );
  }
  const codes = await Promise.all(procs.map((p) => p.exited));
  codes.slice(1).forEach((c, i) => {
    if (c !== 0) console.error(`afl-fuzz secondary s${i + 1} exited ${c}`);
  });
  collectAflCrashes(t);
  return codes[0];
}

function fuzzArgs(t: FuzzTarget, secs: number): string[] {
  const c = CFG[t];
  return [
    corpusDir(t),
    // AFL++ finds, read-only (new libFuzzer units go to the first dir)
    ...aflQueues(t),
    `-artifact_prefix=${crashDir(t)}/`,
    `-max_len=${maxLen(t)}`,
    ...dictArg(t),
    "-rss_limit_mb=4096",
    `-timeout=${c.timeout}`,
    `-report_slow_units=${c.slow}`,
    ...(c.extra ?? []),
    `-max_total_time=${secs}`,
    "-print_final_stats=1",
    // -fork: N child processes, corpus merged back into corpusDir after each
    // job; keep going past findings (each still lands in crashDir).
    ...(jobs > 1 ? [`-fork=${jobs}`, "-ignore_crashes=1", "-ignore_timeouts=1", "-ignore_ooms=1"] : []),
  ];
}

const one = targets.length === 1;
console.log(
  `fuzzing [${targets.join(" ")}] with ${jobs} process(es), ${(one ? [fuzzSans[0]] : fuzzSans).join("/")}` +
    (one ? "" : `, ${slice}s x weight per target per round`) +
    `; corpus=${CORPUS}\n  Ctrl-C to stop; rerun to resume.`,
);

let rc = 0;
let round = 0;
while (!interrupted) {
  round++;
  // one target: a single long run on the first build; otherwise alternate
  const san = fuzzSans[(round - 1) % fuzzSans.length];
  for (const t of targets) {
    if (interrupted) break;
    if (!hasExe(t, san)) continue;
    // A single target just runs until killed (one very long slice).
    const secs = one ? 365 * 24 * 3600 : slice * CFG[t].weight;
    console.log(`\n=== round ${round} (${san}): ${t} for ${secs}s ===`);
    rc = san === "afl" ? await runAfl(t, secs) : await run(fuzzExe(t, san), fuzzArgs(t, secs));
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
