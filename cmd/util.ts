// util.ts -- helpers shared by the build / test scripts in cmd/.
//
// This module imports nothing from the other cmd/ scripts, so anything may
// import it without creating a cycle. It holds: platform flags, the decoder
// source list, the incremental-build primitives (needsRebuild / runCmd), the
// compiler flags for djvudec's own C sources, and a byte formatter.
import { $ } from "bun";
import { existsSync, readdirSync, statSync } from "fs";

// Forward slashes: Bun's shell treats backslashes as escapes, which breaks
// globs and paths in command lines (import.meta.dir is backslashed on Windows).
export const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");

export const isWindows = process.platform === "win32";
export const isMac = process.platform === "darwin";

// On Windows the default toolchain is MSVC; -clang forces clang. Elsewhere only
// clang is available.
export const defaultUseClang = !isWindows;

// The decoder's translation units, in the one order every consumer uses:
// build.ts / build-lib.ts compile + link them in this order, and build-dist.ts
// CONCATENATES them in this order into the amalgamation (djvu_internal.h
// declares every cross-module function, so the order doesn't matter for
// linkage, but it determines the layout of dist/djvu.c -- don't reorder).
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

// Every .c includes these; treat them as inputs so a header edit forces a
// recompile. Without this, changing a struct layout in djvu_internal.h leaves
// stale objects compiled against the old layout -> ABI mismatch at link time.
export const PUBLIC_H = `${ROOT}/src/djvu.h`;
export const INTERNAL_H = `${ROOT}/src/djvu_internal.h`;
export const LIB_HEADERS = [PUBLIC_H, INTERNAL_H];

/** "src/jb2.c" -> "jb2" (object file base name). */
export const objBase = (src: string) =>
  src.replace(/^src\//, "").replace(/\.c$/, "");

/** Executable file name for this platform. */
export function binName(base: string): string {
  return isWindows ? `${base}.exe` : base;
}

/** True when output is missing, or any input is missing or newer than it. */
export function needsRebuild(output: string, ...inputs: string[]): boolean {
  if (!existsSync(output)) return true;
  const outMtime = statSync(output).mtimeMs;
  for (const input of inputs) {
    if (!existsSync(input)) return true;
    if (statSync(input).mtimeMs > outMtime) return true;
  }
  return false;
}

/** Echo a compiler/link command, then run it; throws on a non-zero exit. */
export async function runCmd(cmd: string, cwd?: string): Promise<void> {
  console.log(cwd ? `+ cd ${cwd} && ${cmd}` : `+ ${cmd}`);
  const shell = $`${{ raw: cmd }}`;
  if (cwd) await shell.cwd(cwd);
  else await shell;
}

// MSVC cl.exe / link.exe flags (use '-' not '/' — Bun's shell treats backslashes
// as escapes). Always emit PDBs so WPR/VTune/VS can map stacks on any build.
//
// Compile: -Zi (debug info) + -Fd<dir>/ at each use site (compiler PDB next to
// objects; without -Fd, cl drops vc*.pdb in cwd). -GL is whole-program for LTCG.
// Link: -DEBUG writes the final .pdb next to the .exe; works with -LTCG.
// -Ob3: inline any suitable function (aggressive inlining with /O2).
// Link-only / bench-shim base: no elevated warnings (libdjvu headers, bench_ddjvu.cpp).
export const MSVC_CL_COMMON = `-nologo -O2 -Ob3 -GL -Zi -MT`;
export const MSVC_CL_CXX = `${MSVC_CL_COMMON} -EHsc -std:c++14`;

// Strict warnings for djvudec C sources only (src/*.c, test/*.c, the amalgamation).
// -W4 -WX: high warning level + warnings as errors (C4700/C4701 uninitialized use).
export const DJVUDEC_MSVC_CL_C =
  `${MSVC_CL_COMMON} -W4 -WX -std:c11 -D_CRT_SECURE_NO_WARNINGS`;

/** link.exe flags for MSVC exes: LTCG + full PDB next to the image. */
export const MSVC_LINK = `-LTCG -DEBUG -INCREMENTAL:NO`;

/** Compiler intermediate PDB location (-Fd). Pass the object output directory. */
/* Bun's shell mangles `\`; always emit forward slashes for -Fd. */
export const msvcFd = (dir: string) => `-Fd${dir.replaceAll("\\", "/")}/`;

export const DJVUDEC_CLANG_C_WARN =
  "-Wall -Wextra -Wuninitialized -Wconditional-uninitialized -Winit-self -Werror";

/** clang flags for djvudec C sources only (not libdjvu). */
export function clangCFlags(opt = "-g -O3", win = isWindows): string {
  // _DEFAULT_SOURCE: glibc exposes clock_gettime under -std=c11.
  const crt = win ? " -D_CRT_SECURE_NO_WARNINGS" : " -D_DEFAULT_SOURCE";
  return `-std=c11 ${opt} ${DJVUDEC_CLANG_C_WARN}${crt}`;
}

// Libraries a clang-linked djvudec C program needs. Windows: none. macOS:
// libSystem already provides libm and pthreads. Linux: libm (compose.c uses
// pow) + pthread (the threaded test harnesses).
export const CLANG_C_LINK_LIBS = isWindows || isMac ? "" : "-lpthread -lm";

// "1.2 MB" -- human-readable size.
export function fmtBytesHuman(n: number): string {
  if (n >= 1024 ** 3) return `${(n / 1024 ** 3).toFixed(2)} GB`;
  if (n >= 1024 ** 2) return `${(n / 1024 ** 2).toFixed(1)} MB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${n} B`;
}

// Headers a djvudec C source may include: the two library headers, plus -- for
// the test drivers under test/ -- every header that lives there
// (harness_util.h, winperf_control.h). Used as extra staleness inputs so
// editing a shared header recompiles everything that includes it.
export function headerDeps(src: string): string[] {
  const norm = src.replaceAll("\\", "/");
  if (!norm.includes("/test/") && !norm.startsWith("test/")) return LIB_HEADERS;
  const dir = `${ROOT}/test`;
  const testHeaders = existsSync(dir)
    ? readdirSync(dir)
        .filter((f) => f.endsWith(".h"))
        .map((f) => `${dir}/${f}`)
    : [];
  return [...LIB_HEADERS, ...testHeaders];
}
