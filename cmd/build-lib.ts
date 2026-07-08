// Shared build for library-only tools (djvudec_dump, bench_before, bench_after).
import { $ } from "bun";
import { existsSync, mkdirSync, statSync } from "fs";
import {
  clangCFlags,
  copyAsanRuntimeDll,
  defaultUseClang,
  DJVUDEC_MSVC_CL_C,
  isWindows,
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

type CompileUnit = { src: string; obj: string };

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
  const exePath = `${dir}/${exeFile(target.exeBase, true)}`;
  mkdirSync(dir, { recursive: true });

  // ASan: -O1 for readable traces, like buildAsan in build.ts.
  const cflags = asan
    ? `-fsanitize=address ${clangCFlags("-g -O1")}`
    : clangCFlags();
  const testSrc = target.testSrc ?? `${ROOT}/test/djvudec_dump.c`;
  const units = cUnits(dir, "o", testSrc);
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src)) continue;
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
  const clC = `${DJVUDEC_MSVC_CL_C} -Isrc -Fo${dir}/ -c`;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src)) continue;
    const rel = u.src.startsWith(`${ROOT}/`)
      ? u.src.slice(ROOT.length + 1)
      : u.src;
    await $`cl ${{ raw: clC }} ${{ raw: rel }}`.cwd(ROOT);
  }

  const objs = units.map((u) => u.obj);
  if (needsRebuild(exePath, ...objs)) {
    await $`cl -nologo ${{ raw: objs.join(" ") }} -Fe:${exePath} -link -LTCG`.cwd(ROOT);
  }
  return exePath;
}

export function libToolExePath(
  target: LibToolTarget,
  useClang = defaultUseClang,
  asan = false,
): string {
  return `${toolDir(target, useClang, asan)}/${exeFile(target.exeBase, useClang || asan)}`;
}

export async function buildLibTool(
  target: LibToolTarget,
  useClang = defaultUseClang,
  asan = false,
): Promise<string> {
  if (asan) useClang = true; // ASan builds always use clang
  const name = exeFile(target.exeBase, useClang);
  const exePath = libToolExePath(target, useClang, asan);
  const testSrc = target.testSrc ?? `${ROOT}/test/djvudec_dump.c`;
  const units = cUnits(toolDir(target, useClang, asan), useClang ? "o" : "obj", testSrc);
  const staleObj = units.some((u) => needsRebuild(u.obj, u.src));
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