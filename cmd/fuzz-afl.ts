// fuzz-afl.ts -- coverage-guided fuzzing with AFL++ (macOS / *nix).
//
//   bun cmd/fuzz-afl.ts                  build, seed corpus if empty, fuzz until killed
//   bun cmd/fuzz-afl.ts -target jb2      another target (doc, jb2, iw44, bzz, text, annot, outline)
//   bun cmd/fuzz-afl.ts -jobs 4          1 main + 3 secondary instances (same -o dir)
//   bun cmd/fuzz-afl.ts -repro F         replay a single crash artifact and exit
//   bun cmd/fuzz-afl.ts -import          copy AFL queue/crashes into shared corpus/crashes
//   bun cmd/fuzz-afl.ts -cmin            minimize shared corpus with afl-cmin
//   bun cmd/fuzz-afl.ts -V 60            stop after 60 seconds of fuzz time (smoke / CI)
//
// A second engine next to libFuzzer (bun cmd/fuzz.ts), not a replacement:
// different mutators, and CmpLog (a second binary the main instance runs,
// logging comparison operands so magic values get solved). Both engines share
// the corpus. The libFuzzer rotation also spends a slice on these binaries
// when AFL++ is installed; this script is the long-running campaign.
//   fuzz/corpus/<T>/    seeds for both engines
//   fuzz/afl/<T>/       AFL++ state (queue, hangs, crashes) — gitignored
//   fuzz/crashes/<T>/   tracked regression seeds (libFuzzer + imported AFL crashes)
//
// Same harnesses as libFuzzer: test/fuzz_<T>.c (LLVMFuzzerTestOneInput).
// Built with afl-clang-fast -fsanitize=fuzzer → AFL++ libAFLDriver + persistent
// mode. Resume is automatic when fuzz/afl/<T> already has a campaign.
// The diff target is libFuzzer-only (it links DjVuLibre).
//
// Install: macOS `brew install afl++`, Debian/Ubuntu `sudo apt install afl++`.
//
// macOS once-per-machine setup (raises SysV shm limits; needs root):
//   sudo afl-system-config
// Without that, this script still tries by setting AFL_MAP_SIZE from the binary
// so the coverage map fits under the default 4 MB kern.sysv.shmmax.
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  renameSync,
  rmSync,
  statSync,
} from "node:fs";
import { basename, join, resolve } from "node:path";
import {
  aflAvailable,
  aflTool,
  buildFuzz,
  buildLibTool,
  fuzzExe,
  FUZZ_SEEDS_TARGET,
  FUZZ_TARGETS,
  isFuzzTarget,
  type FuzzTarget,
} from "./build-lib";
import { corpusFiles } from "./corpus";
import { getDeps } from "./get-deps";
import { isMac, isWindows } from "./util";

const ROOT = resolve(import.meta.dir, "..");
export const FUZZ = join(ROOT, "fuzz");
const SEEDS = join(ROOT, "out", "fuzz-seeds");

// Keep in step with CFG in cmd/fuzz.ts. AFL++ MAX_FILE defaults to 1 MB;
// every target here is under that.
const AFL_CFG: Record<Exclude<FuzzTarget, "diff">, { maxLen: number; timeoutMs: number }> = {
  doc: { maxLen: 300_000, timeoutMs: 20_000 },
  jb2: { maxLen: 300_000, timeoutMs: 10_000 },
  iw44: { maxLen: 300_000, timeoutMs: 10_000 },
  bzz: { maxLen: 100_000, timeoutMs: 10_000 },
  text: { maxLen: 100_000, timeoutMs: 10_000 },
  annot: { maxLen: 100_000, timeoutMs: 10_000 },
  outline: { maxLen: 100_000, timeoutMs: 10_000 },
};

const corpusDir = (t: FuzzTarget) => join(FUZZ, "corpus", t);
const crashDir = (t: FuzzTarget) => join(FUZZ, "crashes", t);
export const aflOutDir = (t: FuzzTarget) => join(FUZZ, "afl", t);
const dictPath = (t: FuzzTarget) => join(FUZZ, "dict", `${t}.dict`);

function usage(): never {
  console.error(
    `usage: bun cmd/fuzz-afl.ts [options]
  (no args)      build, seed corpus if empty, afl-fuzz the doc target until
                 killed (resumes)
  -target T      doc (default), jb2, iw44, bzz, text, annot, outline
                 (diff stays on bun cmd/fuzz.ts: it links DjVuLibre)
  -jobs N        parallel AFL instances sharing -o (1 main + N-1 secondaries)
  -max-len N     max input size in bytes (default: the target's; at most 1000000)
  -repro FILE    replay a single crash artifact with the AFL harness and exit
  -import        merge AFL queue → fuzz/corpus/<T> and AFL crashes → fuzz/crashes/<T>
  -cmin          minimize fuzz/corpus/<T> with afl-cmin (writes corpus, keeps backup)
  -no-asan       build without AddressSanitizer and UBSan (faster)
  -no-cmplog     do not build or use the CmpLog binary
  -V SEC         stop after SEC seconds of fuzz time (afl-fuzz -V)
  -clean         force rebuild of the AFL harness
  -h, --help

Corpus is shared with bun cmd/fuzz.ts (fuzz/corpus/<T>/). AFL state lives in
fuzz/afl/<T>/. On macOS run once: sudo afl-system-config
`,
  );
  process.exit(2);
}

function intArg(v: string | undefined, name: string): number {
  const n = Number.parseInt(v ?? "", 10);
  if (!Number.isFinite(n) || n < 1) {
    console.error(`${name} requires a positive integer`);
    process.exit(2);
  }
  return n;
}

function requireTool(name: string): string {
  const p = aflTool(name);
  if (!p) {
    console.error(
      `${name} not found. macOS: brew install afl++; Debian/Ubuntu: sudo apt install afl++`,
    );
    process.exit(2);
  }
  return p;
}

/** List files under dir (non-recursive). */
function listFiles(dir: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir).filter((n) => {
    if (n.startsWith(".")) return false;
    try {
      return statSync(join(dir, n)).isFile();
    } catch {
      return false;
    }
  });
}

/** AFL campaign instance dirs under -o (main, secondary-*, s1, …). */
export function aflInstanceDirs(outDir: string): string[] {
  if (!existsSync(outDir)) return [];
  return readdirSync(outDir)
    .map((n) => join(outDir, n))
    .filter((p) => {
      try {
        return statSync(p).isDirectory() && existsSync(join(p, "queue"));
      } catch {
        return false;
      }
    });
}

function campaignExists(outDir: string): boolean {
  return aflInstanceDirs(outDir).length > 0;
}

function sha1(data: Buffer): string {
  return new Bun.CryptoHasher("sha1").update(data).digest("hex");
}

/**
 * Import AFL-discovered queue entries into the shared libFuzzer corpus, and
 * AFL crashes into fuzz/crashes/<T> (content-hashed names with an afl- prefix,
 * same names the libFuzzer rotation writes).
 *
 * Queue files that still carry `,orig:` are the initial seed copies — skip
 * those so only new coverage goes back to libFuzzer.
 */
export function importAflFindings(t: FuzzTarget): { queue: number; crashes: number } {
  const corpus = corpusDir(t);
  const crashes = crashDir(t);
  mkdirSync(corpus, { recursive: true });
  mkdirSync(crashes, { recursive: true });
  let queue = 0;
  let crashN = 0;
  for (const inst of aflInstanceDirs(aflOutDir(t))) {
    for (const name of listFiles(join(inst, "queue"))) {
      if (name === "README.txt" || name.includes(",orig:")) continue;
      const src = join(inst, "queue", name);
      const hash = sha1(readFileSync(src)).slice(0, 16);
      const dest = join(corpus, `afl-${hash}`);
      if (existsSync(dest)) continue;
      copyFileSync(src, dest);
      queue++;
    }
    for (const name of listFiles(join(inst, "crashes"))) {
      if (name === "README.txt") continue;
      const src = join(inst, "crashes", name);
      const hash = sha1(readFileSync(src)).slice(0, 16);
      const dest = join(crashes, `afl-${hash}`);
      if (existsSync(dest)) continue;
      copyFileSync(src, dest);
      crashN++;
    }
  }
  return { queue, crashes: crashN };
}

/** Coverage-map size the instrumented binary expects (AFL_DUMP_MAP_SIZE). */
export async function dumpMapSize(exe: string): Promise<number | null> {
  const proc = Bun.spawn([exe], {
    env: { ...process.env, AFL_DUMP_MAP_SIZE: "1" },
    stdout: "pipe",
    stderr: "pipe",
  });
  const [stdout, stderr] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  const text = `${stdout}\n${stderr}`;
  const m = text.match(/^\s*(\d+)\s*$/m);
  if (!m) return null;
  const n = Number.parseInt(m[1], 10);
  return Number.isFinite(n) && n > 0 ? n : null;
}

/** Largest map among the harnesses that will run (main + optional CmpLog). */
export async function coverageMapSize(exes: string[]): Promise<number | null> {
  let max = 0;
  for (const exe of exes) {
    const n = await dumpMapSize(exe);
    if (n && n > max) max = n;
  }
  return max > 0 ? max : null;
}

function macShmMax(): number | null {
  if (!isMac) return null;
  try {
    const r = Bun.spawnSync(["sysctl", "-n", "kern.sysv.shmmax"]);
    const n = Number.parseInt(r.stdout.toString().trim(), 10);
    return Number.isFinite(n) ? n : null;
  } catch {
    return null;
  }
}

/** Why AFL++ cannot map its coverage on this machine, or null. */
export function shmProblem(mapSize: number | null): string | null {
  const shmMax = macShmMax();
  const needShm = (mapSize ?? 65536) + 1_000_000 + 4096; // map + fuzz input shm + slack
  if (shmMax !== null && needShm > shmMax) {
    return (
      `macOS SysV shared-memory limit is too small for this target:\n` +
      `  kern.sysv.shmmax=${shmMax}  need≈${needShm}\n` +
      `Raise limits once (requires root):\n` +
      `  sudo afl-system-config\n` +
      `Then re-run. (Also disables ReportCrash delays that mis-label crashes.)`
    );
  }
  return null;
}

export function shmNote(): string | null {
  const shmMax = macShmMax();
  if (isMac && shmMax !== null && shmMax < 8 * 1024 * 1024) {
    return (
      `note: kern.sysv.shmmax=${shmMax} (stock macOS is 4 MiB). ` +
      `Using AFL_MAP_SIZE workaround; for multi-job fuzzing prefer:\n` +
      `  sudo afl-system-config`
    );
  }
  return null;
}

export function baseAflEnv(mapSize: number | null): Record<string, string> {
  const env: Record<string, string> = {};
  for (const [k, v] of Object.entries(process.env)) {
    if (v !== undefined) env[k] = v;
  }
  // macOS: ReportCrash / CPU scaling noise; safe no-ops elsewhere.
  env.AFL_SKIP_CPUFREQ = process.env.AFL_SKIP_CPUFREQ ?? "1";
  env.AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES =
    process.env.AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES ?? "1";
  // Import shared-corpus discoveries from other instances / libFuzzer adds.
  env.AFL_AUTORESUME = process.env.AFL_AUTORESUME ?? "1";
  env.AFL_FAST_CAL = process.env.AFL_FAST_CAL ?? "1";
  env.ASAN_OPTIONS =
    process.env.ASAN_OPTIONS ??
    "abort_on_error=1:detect_leaks=0:symbolize=0:allocator_may_return_null=1";
  // Default AFL shmem is 8 MB; stock macOS kern.sysv.shmmax is 4 MB.
  // Pin map size from the binary so shmget stays under the limit.
  if (mapSize && !process.env.AFL_MAP_SIZE) {
    env.AFL_MAP_SIZE = String(Math.max(mapSize + 1, 65536));
  }
  return env;
}

async function runCapture(argv: string[], env: Record<string, string>): Promise<number> {
  const proc = Bun.spawn(argv, {
    cwd: FUZZ,
    env,
    stdout: "inherit",
    stderr: "inherit",
  });
  return await proc.exited;
}

/** Fill an empty fuzz/corpus/<T> from the deps corpus, same as cmd/fuzz.ts. */
async function seedIfEmpty(t: Exclude<FuzzTarget, "diff">, maxLen: number): Promise<void> {
  const dir = corpusDir(t);
  mkdirSync(dir, { recursive: true });
  if (readdirSync(dir).length > 0) return;
  await getDeps();
  const files = corpusFiles();
  const seedExe = await buildLibTool(FUZZ_SEEDS_TARGET);
  rmSync(SEEDS, { recursive: true, force: true });
  for (const name of FUZZ_TARGETS) mkdirSync(join(SEEDS, name), { recursive: true });
  const proc = Bun.spawn([seedExe, SEEDS, ...files], {
    cwd: ROOT,
    stdout: "inherit",
    stderr: "inherit",
  });
  const rc = await proc.exited;
  if (rc !== 0) {
    console.error(`fuzz_seeds failed (exit ${rc})`);
    process.exit(1);
  }
  let n = 0;
  const src = join(SEEDS, t);
  for (const f of readdirSync(src)) {
    if (statSync(join(src, f)).size > maxLen) continue;
    copyFileSync(join(src, f), join(dir, f));
    n++;
  }
  if (t === "doc") {
    for (const f of files) {
      if (statSync(f).size > maxLen) continue;
      copyFileSync(f, join(dir, basename(f)));
      n++;
    }
  }
  console.log(`seeded ${t} corpus with ${n} input(s)`);
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  let jobs = 1;
  let targetArg = "";
  let maxLenArg = 0;
  let repro = "";
  let doImport = false;
  let cmin = false;
  let noAsan = false;
  let noCmplog = false;
  let timeLimitSec = 0;
  let clean = false;

  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === "-jobs") jobs = intArg(args[++i], "-jobs");
    else if (a === "-target") targetArg = args[++i] ?? usage();
    else if (a === "-max-len") maxLenArg = intArg(args[++i], "-max-len");
    else if (a === "-repro") repro = args[++i] ?? usage();
    else if (a === "-import") doImport = true;
    else if (a === "-cmin") cmin = true;
    else if (a === "-no-asan") noAsan = true;
    else if (a === "-no-cmplog") noCmplog = true;
    else if (a === "-V") timeLimitSec = intArg(args[++i], "-V");
    else if (a === "-clean") clean = true;
    else if (a === "-h" || a === "--help") usage();
    else usage();
  }

  if (isWindows) {
    console.error("AFL++ fuzzing is for macOS/*nix. On Windows use: bun cmd/fuzz.ts");
    process.exit(2);
  }
  if (targetArg === "diff") {
    console.error("the diff target links DjVuLibre and is libFuzzer-only (bun cmd/fuzz.ts -target diff)");
    process.exit(2);
  }
  if (targetArg && !isFuzzTarget(targetArg)) {
    console.error(`unknown target "${targetArg}" (have: ${FUZZ_TARGETS.join(" ")})`);
    process.exit(2);
  }
  const target = (targetArg || "doc") as Exclude<FuzzTarget, "diff">;
  const cfg = AFL_CFG[target];
  const maxLen = Math.min(maxLenArg || cfg.maxLen, 1_000_000);
  const CORPUS = corpusDir(target);
  const CRASHES = crashDir(target);
  const AFL_OUT = aflOutDir(target);

  if (!aflAvailable()) {
    console.error(
      "AFL++ not found (afl-fuzz / afl-clang-fast). macOS: brew install afl++; Debian/Ubuntu: sudo apt install afl++",
    );
    process.exit(2);
  }

  // -import only: merge findings, no rebuild.
  if (doImport && !cmin && !repro) {
    const r = importAflFindings(target);
    console.log(
      `imported ${r.queue} queue input(s) → ${CORPUS}, ${r.crashes} crash(es) → ${CRASHES}`,
    );
    process.exit(0);
  }

  await buildFuzz(clean, [target], "afl", { asan: !noAsan });
  const exe = fuzzExe(target, "afl");
  mkdirSync(CORPUS, { recursive: true });
  mkdirSync(CRASHES, { recursive: true });
  mkdirSync(AFL_OUT, { recursive: true });

  if (repro) {
    const path = resolve(repro);
    if (!existsSync(path)) {
      console.error(`no such file: ${path}`);
      process.exit(1);
    }
    console.log(`replaying ${path}`);
    const mapSize = await dumpMapSize(exe);
    process.exit(await runCapture([exe, path], baseAflEnv(mapSize)));
  }

  await seedIfEmpty(target, maxLen);
  if (readdirSync(CORPUS).length === 0) {
    console.error("corpus is empty; fetch deps (bun cmd/get-deps.ts) and retry");
    process.exit(1);
  }

  if (cmin) {
    const aflCmin = requireTool("afl-cmin");
    const mapSize = await dumpMapSize(exe);
    const problem = shmProblem(mapSize);
    if (problem) {
      console.error(`\n${problem}\n`);
      process.exit(2);
    }
    const env = baseAflEnv(mapSize);
    const tmp = `${CORPUS}.cmin`;
    rmSync(tmp, { recursive: true, force: true });
    mkdirSync(tmp, { recursive: true });
    console.log("minimizing corpus (afl-cmin)...");
    const rc = await runCapture(
      [aflCmin, "-i", CORPUS, "-o", tmp, "-t", String(cfg.timeoutMs), "--", exe],
      env,
    );
    if (rc !== 0) {
      console.error(`afl-cmin failed (${rc}); leaving corpus untouched`);
      rmSync(tmp, { recursive: true, force: true });
      process.exit(rc);
    }
    const before = readdirSync(CORPUS).length;
    const bak = `${CORPUS}.precmin`;
    rmSync(bak, { recursive: true, force: true });
    renameSync(CORPUS, bak);
    renameSync(tmp, CORPUS);
    console.log(
      `corpus minimized: ${before} -> ${readdirSync(CORPUS).length} inputs (backup: ${bak})`,
    );
    process.exit(0);
  }

  let cmplogExe: string | null = null;
  if (!noCmplog) {
    await buildFuzz(clean, [target], "aflcmp");
    cmplogExe = fuzzExe(target, "aflcmp");
  }
  const mapSize = await coverageMapSize(cmplogExe ? [exe, cmplogExe] : [exe]);
  if (mapSize) console.log(`AFL coverage map size: ${mapSize} bytes`);
  const problem = shmProblem(mapSize);
  if (problem) {
    console.error(`\n${problem}\n`);
    process.exit(2);
  }
  const note = shmNote();
  if (note) console.log(note);

  if (campaignExists(AFL_OUT)) {
    const addseeds = aflTool("afl-addseeds");
    if (addseeds) {
      console.log("adding new shared-corpus seeds into existing AFL campaign...");
      await runCapture([addseeds, "-o", AFL_OUT, CORPUS], baseAflEnv(mapSize));
    }
  }

  const aflFuzz = requireTool("afl-fuzz");
  const env = baseAflEnv(mapSize);
  const beforeCrashes = new Set(listFiles(CRASHES));

  // Parent swallows SIGINT so children finish and we can import findings.
  process.on("SIGINT", () => {});

  console.log(
    `AFL++ fuzzing ${target} (${jobs} ${jobs === 1 ? "instance" : "instances"}` +
      `${cmplogExe ? ", cmplog" : ""}); seeds=${CORPUS}\n  state=${AFL_OUT}\n` +
      `  Ctrl-C to stop; rerun to resume; bun cmd/fuzz-afl.ts -target ${target} -import merges queue → shared corpus.`,
  );

  const dict = dictPath(target);
  const children: ReturnType<typeof Bun.spawn>[] = [];

  /** Per-instance -i: resume with '-' only when that instance already has a queue. */
  function spawnAfl(role: "-M" | "-S", name: string, quiet: boolean, cmplog: boolean): ReturnType<typeof Bun.spawn> {
    const resume = existsSync(join(AFL_OUT, name, "queue"));
    const argv = [
      aflFuzz,
      "-i",
      resume ? "-" : CORPUS,
      "-o",
      AFL_OUT,
      role,
      name,
      "-G",
      String(maxLen),
      // Fixed timeout: afl-fuzz's '+' auto-scale often undershoots the slowest
      // dry-run case (same choice as jxldec / heicdec).
      "-t",
      String(cfg.timeoutMs),
      "-m",
      "none",
      // Main keeps importing whatever libFuzzer added to the shared corpus.
      ...(role === "-M" ? ["-F", CORPUS] : []),
      ...(existsSync(dict) ? ["-x", dict] : []),
      // CmpLog on the main, and on one secondary once there are three or more
      // (AFL++: every instance running it would halve throughput).
      ...(cmplog && cmplogExe ? ["-c", cmplogExe] : []),
      ...(timeLimitSec > 0 ? ["-V", String(timeLimitSec)] : []),
      "--",
      exe,
    ];
    return Bun.spawn(argv, {
      cwd: FUZZ,
      env: quiet ? { ...env, AFL_NO_UI: "1" } : env,
      stdout: quiet ? "ignore" : "inherit",
      stderr: quiet ? "ignore" : "inherit",
    });
  }

  // Always name the primary `-M main` so scaling from 1 → N jobs resumes the
  // same instance the libFuzzer rotation uses (an unnamed run would be
  // `-S default` and orphan that queue).
  children.push(spawnAfl("-M", "main", false, true));
  for (let j = 1; j < jobs; j++) {
    const id = `secondary-${String(j).padStart(2, "0")}`;
    children.push(spawnAfl("-S", id, true, j === 1 && jobs >= 3));
  }

  const codes = await Promise.all(children.map((c) => c.exited));
  const rc = codes.find((c) => c !== 0) ?? 0;

  const imported = importAflFindings(target);
  if (imported.queue > 0 || imported.crashes > 0) {
    console.log(
      `\nmerged into shared dirs: ${imported.queue} corpus input(s), ${imported.crashes} crash(es)`,
    );
  }

  const fresh = listFiles(CRASHES).filter((f) => !beforeCrashes.has(f));
  if (fresh.length > 0) {
    console.log(`\n${fresh.length} new crash artifact(s) in ${CRASHES}:`);
    for (const f of fresh) {
      console.log(`  ${f}`);
      console.log(`    reproduce: ${exe} ${join(CRASHES, f)}`);
      console.log(`    or:        bun cmd/fuzz-afl.ts -target ${target} -repro ${join(CRASHES, f)}`);
      console.log(`    or:        bun cmd/fuzz.ts -repro ${join(CRASHES, f)}`);
    }
    process.exit(1);
  }

  process.exit(rc);
}

if (import.meta.main) {
  await main();
}
