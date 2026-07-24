// Shared build for library-only tools (djvudec_dump, bench_before, bench_after).
import { $ } from "bun";
import { existsSync, mkdirSync, rmSync, statSync } from "fs";
import {
  clangCFlags,
  copyAsanRuntimeDll,
  defaultUseClang,
  DJVUDEC_MSVC_CL_C,
  isWindows,
  MSVC_LINK,
  msvcFd,
} from "./build";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");

export const LIB_SRCS = [
  "src/zptable.c",
  "src/zpcodec.c",
  "src/bzz.c",
  "src/bitmap.c",
  "src/jb2.c",
  "src/iw44_zigzag.c",
  "src/iw44.c",
  "src/scaler.c",
  "src/compose.c",
  "src/document.c",
  "src/bufread.c",
  "src/render.c",
  "src/text.c",
  "src/outline.c",
  "src/annot.c",
  "src/debug.c",
];

export type LibToolTarget = {
  /** e.g. out or out/bench_before */
  outRoot: string;
  /** executable base name without .exe */
  exeBase: string;
  /** test driver .c (default test/djvudec_dump.c) */
  testSrc?: string;
};

const objBase = (src: string) => src.replace(/^src\//, "").replace(/\.c$/, "");

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

function needsRebuild(output: string, ...inputs: string[]): boolean {
  if (!existsSync(output)) return true;
  const outMtime = statSync(output).mtimeMs;
  for (const input of inputs) {
    if (!existsSync(input)) return true;
    if (statSync(input).mtimeMs > outMtime) return true;
  }
  return false;
}

// Every .c includes these; treat them as inputs so a header edit forces a
// recompile. Without this, changing a struct layout in djvu_internal.h leaves
// stale objects compiled against the old layout -> ABI mismatch at link time.
const HEADERS = [`${ROOT}/src/djvu.h`, `${ROOT}/src/djvu_internal.h`];

type CompileUnit = { src: string; obj: string };

// An object is stale if older than its source OR any shared header.
const objStale = (u: CompileUnit) => needsRebuild(u.obj, u.src, ...HEADERS);

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
    const link = asan ? "-fsanitize=address " : "";
    await $`clang ${{ raw: link }}${{ raw: objs.join(" ") }} -o ${exePath}`;
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
// out/fuzz/djvudec_fuzz.exe. Driven by cmd/fuzz.ts.
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

  const cflags = `-fsanitize=address,fuzzer ${clangCFlags("-g -O1")}`;
  const staleObj = units.some(objStale);
  const staleExe = needsRebuild(FUZZ_EXE, ...units.map((u) => u.obj));
  if (!staleObj && !staleExe && existsSync(FUZZ_EXE)) {
    // Ensure the runtime DLL even on the up-to-date path: a prior run may have
    // built the exe but failed the copy (idempotent -- no-op if already there).
    if (isWindows) await copyAsanRuntimeDll(FUZZ_DIR);
    console.log("djvudec_fuzz up to date");
    return FUZZ_EXE;
  }

  console.log("building djvudec_fuzz (clang+asan+fuzzer)...");
  for (const u of units) {
    if (!objStale(u)) continue;
    await $`clang ${{ raw: cflags }} -I${ROOT}/src -c -o ${u.obj} ${u.src}`;
  }
  const objs = units.map((u) => u.obj);
  if (needsRebuild(FUZZ_EXE, ...objs)) {
    await $`clang -fsanitize=address,fuzzer ${{ raw: objs.join(" ") }} -o ${FUZZ_EXE}`;
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

export function benchTarget(variant: "before" | "after"): LibToolTarget {
  return {
    outRoot: `${ROOT}/out/bench_${variant}`,
    exeBase: `bench_${variant}`,
  };
}