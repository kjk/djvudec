// Shared build for library-only tools: src/*.c + one test/*.c driver, no
// DjVuLibre (djvudec_dump, djvudec_thread, djvudec_stress, oversize_iw44,
// crash_cache_race, bench_before / bench_after, the libFuzzer target).
import { $ } from "bun";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "fs";
import { buildFuzzOracle, copyAsanRuntimeDll } from "./build";
import {
  CLANG_C_LINK_LIBS,
  clangCFlags,
  defaultUseClang,
  DJVUDEC_MSVC_CL_C,
  isMac,
  isWindows,
  headerDeps,
  LIB_SRCS,
  MSVC_LINK,
  msvcFd,
  needsRebuild,
  objBase,
  ROOT,
} from "./util";

// libFuzzer is not in Apple clang (Xcode). On macOS use Homebrew LLVM
// (`brew install llvm`); Windows keeps the VS-bundled clang. Override with
// DJVUDEC_FUZZ_CLANG=/path/to/clang if needed.
function resolveFuzzClang(): string {
  const override = process.env.DJVUDEC_FUZZ_CLANG;
  if (override) {
    if (!existsSync(override)) {
      throw new Error(`DJVUDEC_FUZZ_CLANG not found: ${override}`);
    }
    return override;
  }
  if (!isMac) return "clang";

  const candidates = [
    process.env.LLVM_PREFIX
      ? `${process.env.LLVM_PREFIX.replaceAll("\\", "/")}/bin/clang`
      : "",
    "/opt/homebrew/opt/llvm/bin/clang",
    "/usr/local/opt/llvm/bin/clang",
  ].filter(Boolean);
  for (const c of candidates) {
    if (existsSync(c)) return c;
  }
  throw new Error(
    "libFuzzer needs Homebrew LLVM on macOS (Apple clang has no fuzzer runtime).\n" +
      "  brew install llvm\n" +
      "Then re-run, or set DJVUDEC_FUZZ_CLANG=/opt/homebrew/opt/llvm/bin/clang",
  );
}

export type LibToolTarget = {
  /** e.g. out or out/bench_before */
  outRoot: string;
  /** executable base name without .exe */
  exeBase: string;
  /** test driver .c (default test/djvudec_dump.c) */
  testSrc?: string;
};

function exeFile(base: string, useClang: boolean): string {
  return useClang && !isWindows ? base : `${base}.exe`;
}

/** Sanitizer for a library-tool build. `true` / `false` mean asan / none. */
export type Sanitizer = "none" | "asan" | "tsan";
type SanArg = Sanitizer | boolean;

function sanOf(s: SanArg): Sanitizer {
  return s === true ? "asan" : s === false ? "none" : s;
}

// Sanitized exes get a distinct name (foo_asan / foo_tsan) and directory
// (out/clang_asan, out/clang_tsan) so a stale plain exe is never mistaken for
// an instrumented one, and vice versa.
function exeBase(target: LibToolTarget, san: SanArg): string {
  const s = sanOf(san);
  return s === "none" ? target.exeBase : `${target.exeBase}_${s}`;
}

function toolDir(target: LibToolTarget, useClang: boolean, san: SanArg = "none"): string {
  const s = sanOf(san);
  return `${target.outRoot}/${s !== "none" ? `clang_${s}` : useClang ? "clang" : "msvc"}`;
}

// clang supports ThreadSanitizer only on Linux / macOS (not the Windows MSVC
// target), so -tsan fails fast there instead of with a cryptic link error.
export function assertTsanSupported(): void {
  if (isWindows) {
    throw new Error(
      "ThreadSanitizer is not supported by clang on Windows; run -tsan on Linux or macOS (CI runs it on Linux)",
    );
  }
}

type CompileUnit = { src: string; obj: string };

// An object is stale if older than its source OR any header it may include.
const objStale = (u: CompileUnit) => needsRebuild(u.obj, u.src, ...headerDeps(u.src));

function cUnits(dir: string, ext: string, testSrc: string): CompileUnit[] {
  const testBase = testSrc.replace(/^.*\//, "").replace(/\.c$/, "");
  return [
    ...LIB_SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${dir}/${objBase(s)}.${ext}`,
    })),
    {
      src: testSrc,
      obj: `${dir}/${testBase}.${ext}`,
    },
  ];
}

async function buildClang(target: LibToolTarget, san: SanArg = "none"): Promise<string> {
  const s = sanOf(san);
  const dir = toolDir(target, true, s);
  const exePath = `${dir}/${exeFile(exeBase(target, s), true)}`;
  mkdirSync(dir, { recursive: true });

  // Sanitizers: -O1 for readable traces, like buildAsan in build.ts.
  const sanFlag = s === "none" ? "" : `-fsanitize=${s === "asan" ? "address" : "thread"} `;
  const cflags = s === "none" ? clangCFlags() : `${sanFlag}${clangCFlags("-g -O1")}`;
  const testSrc = target.testSrc ?? `${ROOT}/test/djvudec_dump.c`;
  const units = cUnits(dir, "o", testSrc);
  for (const u of units) {
    if (!objStale(u)) continue;
    await $`clang ${{ raw: cflags }} -I${ROOT}/src -c -o ${u.obj} ${u.src}`;
  }

  const objs = units.map((u) => u.obj);
  if (needsRebuild(exePath, ...objs)) {
    // Non-Windows/macOS: -lm (compose.c uses pow) + -lpthread (threaded
    // harnesses), the same libs build.ts links djvu_test with on Linux.
    await $`clang ${{ raw: sanFlag }}${{ raw: objs.join(" ") }} ${{ raw: CLANG_C_LINK_LIBS }} -o ${exePath}`;
  }
  if (s === "asan" && isWindows) await copyAsanRuntimeDll(dir);
  return exePath;
}

async function buildMsvc(target: LibToolTarget): Promise<string> {
  const dir = toolDir(target, false);
  const exePath = `${dir}/${exeFile(target.exeBase, false)}`;
  mkdirSync(dir, { recursive: true });

  const testSrc = target.testSrc ?? `${ROOT}/test/djvudec_dump.c`;
  const units = cUnits(dir, "obj", testSrc);
  const clC = `${DJVUDEC_MSVC_CL_C} -Isrc -Fo${dir}/ ${msvcFd(dir)} -c`;
  for (const u of units) {
    if (!objStale(u)) continue;
    const rel = u.src.startsWith(`${ROOT}/`)
      ? u.src.slice(ROOT.length + 1)
      : u.src;
    await $`cl ${{ raw: clC }} ${{ raw: rel }}`.cwd(ROOT);
  }

  const objs = units.map((u) => u.obj);
  if (needsRebuild(exePath, ...objs)) {
    await $`cl -nologo ${{ raw: objs.join(" ") }} -Fe:${exePath} -link ${{ raw: MSVC_LINK }}`.cwd(
      ROOT,
    );
  }
  return exePath;
}

// libFuzzer targets: LIB_SRCS compiled once + one test/fuzz_<name>.c driver
// per target, instrumented with ASan + fuzzer (-O1 for readable traces), no
// main() (libFuzzer provides it). Output out/fuzz/djvudec_fuzz_<name>[.exe].
// Driven by cmd/fuzz.ts. The per-codec targets exist for throughput: a whole
// multi-MB document runs at < 1 exec/s under ASan, a JB2/IW44/BZZ chunk at
// hundreds to thousands.
// diff = differential (ours vs DjVuLibre on the same input; links libdjvu).
export const FUZZ_TARGETS = ["doc", "jb2", "iw44", "bzz", "text", "annot", "outline", "diff"] as const;
export type FuzzTarget = (typeof FUZZ_TARGETS)[number];
export const isFuzzTarget = (s: string): s is FuzzTarget =>
  (FUZZ_TARGETS as readonly string[]).includes(s);

// Sanitizer builds of the fuzz targets, each in its own dir so they coexist:
//   asan  out/fuzz       ASan (+ UBSan, non-recoverable, on Linux/macOS --
//                        Windows ASan+UBSan is flaky with the VS clang
//                        runtime; DJVU_FUZZ_UBSAN=0 turns UBSan off)
//   msan  out/fuzz_msan  MemorySanitizer with origin tracking (Linux only):
//                        uninitialized reads ASan cannot see. The diff target
//                        is not built for it (the uninstrumented libdjvu
//                        oracle would report false positives).
//   cov   out/fuzz_cov   no sanitizer; clang source-based coverage
//                        (-fprofile-instr-generate -fcoverage-mapping), for
//                        `fuzz.ts -coverage` reports. libFuzzer only
//                        supplies main() to replay the corpus.
//   afl    out/fuzz_afl    AFL++ (afl-clang-fast, libFuzzer harness via its
//                         libAFLDriver, persistent mode) with ASan + UBSan
//   aflcmp out/fuzz_aflcmp AFL++ CmpLog companion (AFL_LLVM_CMPLOG, no
//                         sanitizer), passed to afl-fuzz -c
//   Neither builds the diff target.
export type FuzzSan = "asan" | "msan" | "cov" | "afl" | "aflcmp";
const isAflSan = (s: FuzzSan) => s === "afl" || s === "aflcmp";
/** Prefer Homebrew AFL++ wrappers (afl-clang-fast → Homebrew LLVM). */
export function aflTool(name: string): string | null {
  const envKey = name.toUpperCase().replaceAll("-", "_");
  const env = process.env[envKey];
  if (env && existsSync(env)) return env;
  for (const base of ["/opt/homebrew/bin", "/usr/local/bin"]) {
    const p = `${base}/${name}`;
    if (existsSync(p)) return p;
  }
  return Bun.which(name);
}

export function findAflCc(): string {
  const env = process.env.AFL_CC || process.env.AFL_CLANG_FAST;
  if (env && existsSync(env)) return env;
  return aflTool("afl-clang-fast") || aflTool("afl-cc") || "afl-clang-fast";
}

// AFL++ is Linux/macOS only. Homebrew's binaries live outside the default
// PATH of some shells, so look there before `which`.
export const aflAvailable = (): boolean => {
  if (isWindows || !aflTool("afl-fuzz")) return false;
  const cc = findAflCc();
  return cc.includes("/") ? existsSync(cc) : !!Bun.which(cc);
};
export const msanSupported = !isWindows && !isMac;
const fuzzDir = (san: FuzzSan) => `${ROOT}/out/${san === "asan" ? "fuzz" : `fuzz_${san}`}`;
const FUZZ_DIR = fuzzDir("asan");
export const ubsanEnabled = () => !isWindows && process.env.DJVU_FUZZ_UBSAN !== "0";

// Numeric kernels (wavelet transform, color conversion, scaling, compositing)
// keep edge coverage but drop -fsanitize=fuzzer's comparison tracing: a perf
// profile of the doc target spent ~45% of its time in
// __sanitizer_cov_trace_*cmp* called from these loops, where cmp feedback
// (solving magic values) buys nothing.
const FUZZ_NO_TRACE_CMP = new Set(["src/iw44.c", "src/scaler.c", "src/compose.c"]);
export function fuzzExe(t: FuzzTarget, san: FuzzSan = "asan"): string {
  return `${fuzzDir(san)}/djvudec_fuzz_${t}${isWindows ? ".exe" : ""}`;
}

// Always a clean build when clean=true (fuzz.ts passes it): build-lib tracks
// sources and headers but not compile FLAGS, and the fuzz flags vary by
// environment (sanitizer set, compiler) -- see the cmd/fuzz.ts header.
export async function buildFuzz(
  clean = false,
  targets: readonly FuzzTarget[] = FUZZ_TARGETS,
  san: FuzzSan = "asan",
  // asan: ASan+UBSan on the AFL++ harness (default). Ignored for aflcmp,
  // which AFL++ wants unsanitized. A stamp file rebuilds when this flips.
  opts: { asan?: boolean } = {},
): Promise<void> {
  if (san === "msan") {
    if (!msanSupported) throw new Error("MemorySanitizer needs Linux (clang has no MSan on Windows/macOS)");
    targets = targets.filter((t) => t !== "diff");
  }
  if (isAflSan(san)) {
    if (!aflAvailable()) {
      throw new Error(
        "AFL++ not found (afl-fuzz / afl-clang-fast). macOS: brew install afl++; Debian/Ubuntu: sudo apt install afl++",
      );
    }
    targets = targets.filter((t) => t !== "diff");
  }
  const FUZZ_DIR = fuzzDir(san);
  const fuzzExeS = (t: FuzzTarget) => fuzzExe(t, san);
  mkdirSync(FUZZ_DIR, { recursive: true });
  const libUnits = LIB_SRCS.map((s) => ({
    src: `${ROOT}/${s}`,
    obj: `${FUZZ_DIR}/${objBase(s)}.o`,
    extra: FUZZ_NO_TRACE_CMP.has(s) && !isAflSan(san) ? "-fno-sanitize-coverage=trace-cmp" : "",
  }));
  const tgtUnits = targets.map((t) => ({
    t,
    src: `${ROOT}/test/fuzz_${t}.c`,
    obj: `${FUZZ_DIR}/fuzz_${t}.o`,
    extra: "",
  }));
  // afl-clang-fast reads its mode from the environment (at compile AND link)
  // and turns -fsanitize=fuzzer into its own driver.
  const clang = isAflSan(san) ? findAflCc() : resolveFuzzClang();
  const aflAsan = san === "afl" && opts.asan !== false;
  const env: Record<string, string | undefined> = { ...process.env, AFL_QUIET: "1" };
  let stampChanged = false;
  let stampPath = "";
  let stampWant = "";
  if (isAflSan(san)) {
    // AFL plugin warnings become errors under -Werror on some LLVM versions.
    env.AFL_LLVM_NO_ERROR = process.env.AFL_LLVM_NO_ERROR ?? "1";
    if (aflAsan) {
      env.AFL_USE_ASAN = "1";
      if (ubsanEnabled()) env.AFL_USE_UBSAN = "1";
      else delete env.AFL_USE_UBSAN;
    } else {
      delete env.AFL_USE_ASAN;
      delete env.AFL_USE_UBSAN;
    }
    if (san === "aflcmp") env.AFL_LLVM_CMPLOG = "1";
    else delete env.AFL_LLVM_CMPLOG;
    stampWant =
      `asan=${aflAsan ? 1 : 0};ubsan=${aflAsan && ubsanEnabled() ? 1 : 0};` +
      `cmplog=${san === "aflcmp" ? 1 : 0};cc=${clang}`;
    stampPath = `${FUZZ_DIR}/.afl_features`;
    const stampPrev = existsSync(stampPath) ? readFileSync(stampPath, "utf8").trim() : "";
    stampChanged = stampPrev !== stampWant;
  }
  if (clean || stampChanged) {
    for (const u of [...libUnits, ...tgtUnits]) rmSync(u.obj, { force: true });
    for (const t of targets) rmSync(fuzzExeS(t), { force: true });
  }
  // UBSan non-recoverable so a finding aborts and libFuzzer saves the input
  // (recoverable UBSan only logs, which a fuzzer never notices).
  const sanitize =
    isAflSan(san)
      ? "fuzzer"
      : san === "cov"
      ? "fuzzer -fprofile-instr-generate -fcoverage-mapping"
      : san === "msan"
      ? "memory,fuzzer -fsanitize-memory-track-origins"
      : ubsanEnabled()
        ? "address,undefined,fuzzer -fno-sanitize-recover=undefined"
        : "address,fuzzer";
  // DJVU_FUZZ_HOOKS: test-only instrumentation in the library (debug.c /
  // DJVU_DEBUG_ONLY regions), e.g. the ZP EndOfFile counter fuzz_diff uses.
  const cflags = `-fsanitize=${sanitize} -DDJVU_FUZZ_HOOKS ${clangCFlags("-g -O1")}`;
  const all = [...libUnits, ...tgtUnits];
  const stale = all.filter(objStale);
  const libObjs = libUnits.map((u) => u.obj);
  const staleExes = tgtUnits.filter(
    (u) =>
      stale.length > 0 ||
      needsRebuild(fuzzExeS(u.t), u.obj, ...libObjs) ||
      (u.t === "diff" && needsRebuild(fuzzExeS(u.t), `${ROOT}/test/fuzz_ddjvu.cpp`)) ||
      !existsSync(fuzzExeS(u.t)),
  );
  if (staleExes.length === 0) {
    // Ensure the runtime DLL even on the up-to-date path: a prior run may have
    // built the exe but failed the copy (idempotent -- no-op if already there).
    if (isWindows && san === "asan") await copyAsanRuntimeDll(FUZZ_DIR);
    console.log("fuzz targets up to date");
    return;
  }

  console.log(`building fuzz targets [${targets.join(" ")}] (clang+${sanitize}; ${clang})...`);
  await Promise.all(
    stale.map((u) => $`${clang} ${{ raw: cflags }} ${{ raw: u.extra }} -I${ROOT}/src -c -o ${u.obj} ${u.src}`.env(env)),
  );
  for (const u of staleExes) {
    if (u.t === "diff") {
      // C++ link: the DjVuLibre oracle (uninstrumented shim + libdjvu).
      const oracle = await buildFuzzOracle(FUZZ_DIR);
      const clangxx = clang.replace(/clang(\.exe)?$/, "clang++$1");
      await $`${clangxx} -fsanitize=${{ raw: sanitize }} ${{ raw: [...libObjs, u.obj, ...oracle.objs].join(" ") }} ${{ raw: oracle.linkLibs }} -o ${fuzzExeS(u.t)}`;
      continue;
    }
    await $`${clang} -fsanitize=${{ raw: sanitize }} ${{ raw: [...libObjs, u.obj].join(" ") }} ${{ raw: CLANG_C_LINK_LIBS }} -o ${fuzzExeS(u.t)}`.env(env);
  }
  if (isWindows && san === "asan") await copyAsanRuntimeDll(FUZZ_DIR);
  if (stampPath) writeFileSync(stampPath, stampWant);
  console.log("built fuzz targets");
}

// An LLVM tool (llvm-profdata, llvm-cov) matching the fuzz clang: next to it,
// on PATH, or versioned (Ubuntu's llvm-cov-18). null if not found.
export function llvmTool(name: string): string | null {
  const exe = isWindows ? ".exe" : "";
  const clang = resolveFuzzClang();
  const clangPath = clang.includes("/") ? clang : Bun.which(clang);
  const cands: string[] = [];
  if (clangPath) cands.push(`${clangPath.replace(/[^/\\]*$/, "")}${name}${exe}`);
  cands.push(name);
  const ver = Bun.spawnSync([clang, "-dumpversion"]).stdout.toString().trim().split(".")[0];
  if (ver) cands.push(`${name}-${ver}`);
  for (const c of cands) {
    if (c.includes("/") ? existsSync(c) : Bun.which(c)) return c;
  }
  return null;
}

// Seed extractor (test/fuzz_seeds.c): plain build, run by fuzz.ts.
export const FUZZ_SEEDS_TARGET: LibToolTarget = {
  outRoot: `${ROOT}/out`,
  exeBase: "fuzz_seeds",
  testSrc: `${ROOT}/test/fuzz_seeds.c`,
};

export function libToolExePath(
  target: LibToolTarget,
  useClang = defaultUseClang,
  san: SanArg = "none",
): string {
  const s = sanOf(san);
  return `${toolDir(target, useClang, s)}/${exeFile(exeBase(target, s), useClang || s !== "none")}`;
}

export async function buildLibTool(
  target: LibToolTarget,
  useClang = defaultUseClang,
  sanArg: SanArg = "none",
  clean = false,
): Promise<string> {
  const san = sanOf(sanArg);
  if (san === "tsan") assertTsanSupported();
  if (san !== "none") useClang = true; // sanitizer builds always use clang
  const name = exeFile(exeBase(target, san), useClang);
  const exePath = libToolExePath(target, useClang, san);
  const testSrc = target.testSrc ?? `${ROOT}/test/djvudec_dump.c`;
  const units = cUnits(toolDir(target, useClang, san), useClang ? "o" : "obj", testSrc);
  if (clean) {
    for (const u of units) rmSync(u.obj, { force: true });
    rmSync(exePath, { force: true });
  }
  const staleObj = units.some(objStale);
  const staleExe = needsRebuild(exePath, ...units.map((u) => u.obj));

  if (!staleObj && !staleExe && existsSync(exePath)) {
    console.log(`${name} up to date`);
    return exePath;
  }

  const what = san !== "none" ? `clang+${san}` : useClang ? "clang" : "msvc";
  console.log(`building ${name} (${what})...`);
  const exe = useClang ? await buildClang(target, san) : await buildMsvc(target);
  console.log(`built ${name}`);
  return exe;
}

export const DUMP_TARGET: LibToolTarget = {
  outRoot: `${ROOT}/out`,
  exeBase: "djvudec_dump",
};

export const THREAD_TARGET: LibToolTarget = {
  outRoot: `${ROOT}/out`,
  exeBase: "djvudec_thread",
  testSrc: `${ROOT}/test/djvudec_thread.c`,
};

export const STRESS_TARGET: LibToolTarget = {
  outRoot: `${ROOT}/out`,
  exeBase: "djvudec_stress",
  testSrc: `${ROOT}/test/djvudec_stress.c`,
};

// test/crash_cache_race.c: concurrent render vs djvu_doc_drop_page_cache
// regression test (run by cmd/test-cache-race.ts).
export const CACHE_RACE_TARGET: LibToolTarget = {
  outRoot: `${ROOT}/out`,
  exeBase: "crash_cache_race",
  testSrc: `${ROOT}/test/crash_cache_race.c`,
};

export const OVERSIZE_IW44_TARGET: LibToolTarget = {
  outRoot: `${ROOT}/out`,
  exeBase: "oversize_iw44",
  testSrc: `${ROOT}/test/oversize_iw44.c`,
};

export function benchTarget(variant: "before" | "after"): LibToolTarget {
  return {
    outRoot: `${ROOT}/out/bench_${variant}`,
    exeBase: `bench_${variant}`,
  };
}

// The exe a before/after comparison should run (bench-perf.ts, profile-render.ts).
//   after:  built / refreshed from the current sources, as usual.
//   before: a SNAPSHOT of the sources at the time `bun cmd/build-bench.ts before`
//           ran. It is never (re)built here: its objects are older than the
//           edited src/ by design, so an implicit incremental build would
//           silently recompile it from the edited sources and the comparison
//           would be after-vs-after. Missing snapshot -> error.
export async function benchExe(
  variant: "before" | "after",
  useClang = defaultUseClang,
): Promise<string> {
  if (variant === "after") return buildLibTool(benchTarget("after"), useClang);
  const exe = libToolExePath(benchTarget("before"), useClang);
  if (!existsSync(exe)) {
    const tc = useClang && isWindows ? " -clang" : "";
    throw new Error(
      `no "before" snapshot at ${exe}\n` +
        `Build it from the baseline sources first (before editing src/):\n` +
        `  bun cmd/build-bench.ts before${tc}\n` +
        `It is never rebuilt implicitly; only build-bench.ts (re)builds it.`,
    );
  }
  return exe;
}