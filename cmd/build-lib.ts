// Shared build for library-only tools: src/*.c + one test/*.c driver, no
// DjVuLibre (djvudec_dump, djvudec_thread, djvudec_stress, oversize_iw44,
// crash_cache_race, bench_before / bench_after, the libFuzzer target).
import { $ } from "bun";
import { existsSync, mkdirSync, rmSync } from "fs";
import { copyAsanRuntimeDll } from "./build";
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

// ASan exes get a distinct name (foo_asan) so a stale plain exe is never
// mistaken for an instrumented one, and vice versa.
function exeBase(target: LibToolTarget, asan: boolean): string {
  return asan ? `${target.exeBase}_asan` : target.exeBase;
}

function toolDir(target: LibToolTarget, useClang: boolean, asan = false): string {
  return `${target.outRoot}/${asan ? "clang_asan" : useClang ? "clang" : "msvc"}`;
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

async function buildClang(target: LibToolTarget, asan = false): Promise<string> {
  const dir = toolDir(target, true, asan);
  const exePath = `${dir}/${exeFile(exeBase(target, asan), true)}`;
  mkdirSync(dir, { recursive: true });

  // ASan: -O1 for readable traces, like buildAsan in build.ts.
  const cflags = asan
    ? `-fsanitize=address ${clangCFlags("-g -O1")}`
    : clangCFlags();
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
    const link = asan ? "-fsanitize=address " : "";
    await $`clang ${{ raw: link }}${{ raw: objs.join(" ") }} ${{ raw: CLANG_C_LINK_LIBS }} -o ${exePath}`;
  }
  if (asan && isWindows) await copyAsanRuntimeDll(dir);
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

// libFuzzer target: LIB_SRCS + test/fuzz_target.c, instrumented with ASan +
// fuzzer (-O1 for readable traces), no main() (libFuzzer provides it). Output
// out/fuzz/djvudec_fuzz[.exe]. Driven by cmd/fuzz.ts.
const FUZZ_DIR = `${ROOT}/out/fuzz`;
export const FUZZ_EXE = `${FUZZ_DIR}/${isWindows ? "djvudec_fuzz.exe" : "djvudec_fuzz"}`;

export async function buildFuzz(clean = false): Promise<string> {
  mkdirSync(FUZZ_DIR, { recursive: true });
  const testSrc = `${ROOT}/test/fuzz_target.c`;
  const units = cUnits(FUZZ_DIR, "o", testSrc);
  if (clean) {
    for (const u of units) rmSync(u.obj, { force: true });
    rmSync(FUZZ_EXE, { force: true });
  }

  const clang = resolveFuzzClang();
  // DJVU_FUZZ_UBSAN=1 adds undefined sanitizer (Linux/macOS CI; Windows
  // ASan+UBSan together is flaky with the VS clang runtime).
  const wantUbsan =
    process.env.DJVU_FUZZ_UBSAN === "1" || process.env.DJVU_FUZZ_UBSAN === "true";
  const sanitize =
    wantUbsan && !isWindows ? "address,undefined,fuzzer" : "address,fuzzer";
  const cflags = `-fsanitize=${sanitize} ${clangCFlags("-g -O1")}`;
  const staleObj = units.some(objStale);
  const staleExe = needsRebuild(FUZZ_EXE, ...units.map((u) => u.obj));
  if (!staleObj && !staleExe && existsSync(FUZZ_EXE)) {
    // Ensure the runtime DLL even on the up-to-date path: a prior run may have
    // built the exe but failed the copy (idempotent -- no-op if already there).
    if (isWindows) await copyAsanRuntimeDll(FUZZ_DIR);
    console.log("djvudec_fuzz up to date");
    return FUZZ_EXE;
  }

  console.log(`building djvudec_fuzz (clang+${sanitize}; ${clang})...`);
  for (const u of units) {
    if (!objStale(u)) continue;
    await $`${clang} ${{ raw: cflags }} -I${ROOT}/src -c -o ${u.obj} ${u.src}`;
  }
  const objs = units.map((u) => u.obj);
  if (needsRebuild(FUZZ_EXE, ...objs)) {
    await $`${clang} -fsanitize=${sanitize} ${{ raw: objs.join(" ") }} ${{ raw: CLANG_C_LINK_LIBS }} -o ${FUZZ_EXE}`;
  }
  if (isWindows) await copyAsanRuntimeDll(FUZZ_DIR);
  console.log("built djvudec_fuzz");
  return FUZZ_EXE;
}

export function libToolExePath(
  target: LibToolTarget,
  useClang = defaultUseClang,
  asan = false,
): string {
  return `${toolDir(target, useClang, asan)}/${exeFile(exeBase(target, asan), useClang || asan)}`;
}

export async function buildLibTool(
  target: LibToolTarget,
  useClang = defaultUseClang,
  asan = false,
  clean = false,
): Promise<string> {
  if (asan) useClang = true; // ASan builds always use clang
  const name = exeFile(exeBase(target, asan), useClang);
  const exePath = libToolExePath(target, useClang, asan);
  const testSrc = target.testSrc ?? `${ROOT}/test/djvudec_dump.c`;
  const units = cUnits(toolDir(target, useClang, asan), useClang ? "o" : "obj", testSrc);
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

  console.log(`building ${name} (${asan ? "clang+asan" : useClang ? "clang" : "msvc"})...`);
  const exe = useClang ? await buildClang(target, asan) : await buildMsvc(target);
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