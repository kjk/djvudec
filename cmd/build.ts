// build.ts -- build driver for the djvu C port (run with `bun cmd/build.ts`).
//
//   bun cmd/build.ts          fetch deps + ref tools + the harness (MSVC default)
//   bun cmd/build.ts -clang   build the harness with clang instead of MSVC
//   bun cmd/build.ts -clean   delete out/ before building (full rebuild)
//   bun cmd/build.ts ref      (re)build the DjVuLibre reference tools
//
// Harness objects and exes land in out/msvc/ or out/clang/. The build is
// incremental: a source is recompiled only when newer than its object, and the
// exe is relinked only when an object or libdjvu.lib is newer than the exe.
// build() returns the exe path. Verification lives in tests.ts.
import { $ } from "bun";
import { existsSync, mkdirSync, rmSync, statSync } from "fs";
import { DIST_C, DIST_H, ensureDist } from "./build-dist";
import { DJVULIBRE_DIR, getDeps } from "./get-deps";

// Forward slashes: Bun's shell treats backslashes as escapes, which breaks the
// *.cpp / *.o globs below (import.meta.dir is backslashed on Windows).
const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const DJVULIBRE = DJVULIBRE_DIR.replaceAll("\\", "/"); // deps/ checkout (see get-deps.ts)
const OUT_ROOT = `${ROOT}/out`;
const REF = `${ROOT}/ref_build`;
const OBJDIR = `${REF}/djvuobj`;

export const isWindows = process.platform === "win32";
export const isMac = process.platform === "darwin";

// Windows: libdjvu.lib + llvm-lib; macOS: libdjvu.a + ar.
const LIBDJVU = `${REF}/${isWindows ? "libdjvu.lib" : "libdjvu.a"}`;

const outDir = (useClang: boolean) =>
  `${OUT_ROOT}/${useClang ? "clang" : "msvc"}`;

function binName(base: string): string {
  return isWindows ? `${base}.exe` : base;
}

// DjVuLibre C++ compile flags (oracle only — NOT our code; keep -w, no -Werror).
// -O3: both sides are built at max optimization so `-bench` is a fair compare.
const DJVU_CXXFLAGS_WIN =
  `-std=c++14 -w -O3 -DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

const DJVU_CXXFLAGS_MAC =
  `-std=c++14 -w -O3 -DAUTOCONF -DHAVE_STDINCLUDES ` +
  `-DHAVE_NAMESPACES -DHAVE_PTHREAD -DHAVE_STDINT_H -DHAVE_WCHAR_H ` +
  `-DHAVE_STRERROR -DHAVE_DIRENT_H -DHAVE_SYS_TIME_H ` +
  `-DHAS_WCHAR=1 -DHAS_WCTYPE=1 -DHAS_MBSTATE=1 -DUNIX ` +
  `-DDIR_DATADIR='"/usr/share"' ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

const djvuLinkLibs = () => (isWindows ? "-ladvapi32" : "-lpthread");

const INTERNAL_H = `${ROOT}/src/djvu_internal.h`;
const PUBLIC_H = `${ROOT}/src/djvu.h`;

const SRCS = [
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

const objBase = (src: string) => src.replace(/^src\//, "").replace(/\.c$/, "");

function needsRebuild(output: string, ...inputs: string[]): boolean {
  if (!existsSync(output)) return true;
  const outMtime = statSync(output).mtimeMs;
  for (const input of inputs) {
    if (!existsSync(input)) return true;
    if (statSync(input).mtimeMs > outMtime) return true;
  }
  return false;
}

/** Echo a compiler/link command, then run it (stdout progress tracking). */
async function runCmd(cmd: string, cwd?: string): Promise<void> {
  console.log(cwd ? `+ cd ${cwd} && ${cmd}` : `+ ${cmd}`);
  const shell = $`${{ raw: cmd }}`;
  if (cwd) await shell.cwd(cwd);
  else await shell;
}

export function cleanBuildOutput(): void {
  rmSync(OUT_ROOT, { recursive: true, force: true });
}

export function refToolPath(tool: string): string {
  return `${REF}/${binName(tool)}`;
}

// Build ddjvu.exe / djvutxt.exe from DjVuLibre (static, decode oracle).
async function buildRefWindows() {
  mkdirSync(REF, { recursive: true });
  const common = DJVU_CXXFLAGS_WIN;
  const libsrc = `${DJVULIBRE}/libdjvu/*.cpp`;
  for (const tool of ["ddjvu", "djvutxt", "bzz", "djvused"]) {
    const exe = `${REF}/${tool}.exe`;
    if (existsSync(exe)) continue;
    await runCmd(
      `clang++ ${common} ${libsrc} ${DJVULIBRE}/tools/${tool}.cpp -ladvapi32 -o ${exe}`,
    );
  }
  if (!existsSync(`${REF}/iw44ref.exe`)) {
    await runCmd(
      `clang++ ${common} ${libsrc} ${ROOT}/test/iw44ref.cpp -ladvapi32 -o ${REF}/iw44ref.exe`,
    );
  }
  if (!existsSync(`${REF}/jb2ref.exe`)) {
    await runCmd(
      `clang++ ${common} ${libsrc} ${ROOT}/test/jb2ref.cpp -ladvapi32 -o ${REF}/jb2ref.exe`,
    );
  }
  console.log("ref tools ready");
}

async function buildRefMac() {
  mkdirSync(REF, { recursive: true });
  const common = DJVU_CXXFLAGS_MAC;
  const link = djvuLinkLibs();
  const libsrc = `${DJVULIBRE}/libdjvu/*.cpp`;
  for (const tool of ["ddjvu", "djvutxt", "bzz", "djvused"]) {
    const exe = refToolPath(tool);
    if (existsSync(exe)) continue;
    await runCmd(
      `clang++ ${common} ${libsrc} ${DJVULIBRE}/tools/${tool}.cpp ${link} -o ${exe}`,
    );
  }
  if (!existsSync(refToolPath("iw44ref"))) {
    await runCmd(
      `clang++ ${common} ${libsrc} ${ROOT}/test/iw44ref.cpp ${link} -o ${refToolPath("iw44ref")}`,
    );
  }
  if (!existsSync(refToolPath("jb2ref"))) {
    await runCmd(
      `clang++ ${common} ${libsrc} ${ROOT}/test/jb2ref.cpp ${link} -o ${refToolPath("jb2ref")}`,
    );
  }
  console.log("ref tools ready");
}

export async function buildRef() {
  if (isWindows) return buildRefWindows();
  if (isMac) return buildRefMac();
  throw new Error(`unsupported platform: ${process.platform}`);
}

// Compile all of libdjvu into a cached static lib (one-time, slow) so djvu_test
// can link DjVuLibre's decoder for `-bench` without recompiling it every build.
async function buildLibDjvuWindows() {
  if (existsSync(LIBDJVU)) return;
  console.log("building libdjvu.lib (one-time, slow)...");
  mkdirSync(OBJDIR, { recursive: true });
  await runCmd(`clang++ ${DJVU_CXXFLAGS_WIN} -c ${DJVULIBRE}/libdjvu/*.cpp`, OBJDIR);
  await runCmd(`llvm-lib /out:${LIBDJVU} *.o`, OBJDIR);
  console.log("built libdjvu.lib");
}

async function buildLibDjvuMac() {
  if (existsSync(LIBDJVU)) return;
  console.log("building libdjvu.a (one-time, slow)...");
  mkdirSync(OBJDIR, { recursive: true });
  await runCmd(`clang++ ${DJVU_CXXFLAGS_MAC} -c ${DJVULIBRE}/libdjvu/*.cpp`, OBJDIR);
  await runCmd(`ar rcs ${LIBDJVU} *.o`, OBJDIR);
  console.log("built libdjvu.a");
}

export async function buildLibDjvu() {
  if (isWindows) return buildLibDjvuWindows();
  if (isMac) return buildLibDjvuMac();
  throw new Error(`unsupported platform: ${process.platform}`);
}

// DjVuLibre include + define flags shared by the clang and cl shim builds.
const DJVU_DEFINES =
  `-DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

// On Windows the default toolchain is MSVC; -clang forces clang. Elsewhere only
// clang is available.
export const defaultUseClang = !isWindows;

// MSVC cl.exe flags (use '-' not '/' — Bun's shell treats backslashes as escapes).
// -Ob3: inline any suitable function (aggressive inlining with /O2).
// Link-only / bench-shim base: no elevated warnings (libdjvu headers, bench_ddjvu.cpp).
export const MSVC_CL_COMMON = `-nologo -O2 -Ob3 -GL -MT`;
export const MSVC_CL_CXX = `${MSVC_CL_COMMON} -EHsc -std:c++14`;

// Strict warnings for djvudec C sources only (src/*.c, test/*.c, dist/djvu.c).
// -W4 -WX: high warning level + warnings as errors (C4700/C4701 uninitialized use).
export const DJVUDEC_MSVC_CL_C =
  `${MSVC_CL_COMMON} -W4 -WX -std:c11 -D_CRT_SECURE_NO_WARNINGS`;

export const DJVUDEC_CLANG_C_WARN =
  "-Wall -Wextra -Wuninitialized -Wconditional-uninitialized -Winit-self -Werror";
const DJVUDEC_CLANG_C_STD = "-std=c11";
const DJVUDEC_CLANG_C_DEFINES_WIN = "-D_CRT_SECURE_NO_WARNINGS";

/** clang flags for djvudec C sources only (not libdjvu). */
export function clangCFlags(opt = "-g -O3", win = isWindows): string {
  const crt = win ? ` ${DJVUDEC_CLANG_C_DEFINES_WIN}` : "";
  return `${DJVUDEC_CLANG_C_STD} ${opt} ${DJVUDEC_CLANG_C_WARN}${crt}`;
}
const harnessExeName = (useClang: boolean) =>
  binName(`djvu_test_${useClang ? "clang" : "msvc"}`);

type CompileUnit = { src: string; obj: string; label: string };

function cUnits(dir: string, ext: string): CompileUnit[] {
  return [
    ...SRCS.map((s) => {
      const base = objBase(s);
      return {
        src: `${ROOT}/${s}`,
        obj: `${dir}/${base}.${ext}`,
        label: s,
      };
    }),
    {
      src: `${ROOT}/test/djvu_test.c`,
      obj: `${dir}/djvu_test.${ext}`,
      label: "test/djvu_test.c",
    },
  ];
}

// Build the harness with clang -> out/clang/djvu_test_clang[.exe].
async function buildClangWindows(): Promise<string> {
  const dir = outDir(true);
  const exe = harnessExeName(true);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const units = cUnits(dir, "o");
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_clang.o`,
    label: "test/bench_ddjvu.cpp",
  };

  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    await runCmd(`clang ${clangCFlags()} -I${ROOT}/src -c -o ${u.obj} ${u.src}`);
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await runCmd(`clang++ ${DJVU_CXXFLAGS_WIN} -c -o ${bench.obj} ${bench.src}`);
  }

  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await runCmd(`clang++ ${objs.join(" ")} ${LIBDJVU} -ladvapi32 -o ${exePath}`);
  }
  return exePath;
}

async function buildClangMac(): Promise<string> {
  const dir = outDir(true);
  const exe = harnessExeName(true);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const units = cUnits(dir, "o");
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_clang.o`,
    label: "test/bench_ddjvu.cpp",
  };

  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    await runCmd(`clang ${clangCFlags()} -I${ROOT}/src -c -o ${u.obj} ${u.src}`);
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await runCmd(`clang++ ${DJVU_CXXFLAGS_MAC} -c -o ${bench.obj} ${bench.src}`);
  }

  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await runCmd(`clang++ ${objs.join(" ")} ${LIBDJVU} ${djvuLinkLibs()} -o ${exePath}`);
  }
  return exePath;
}

async function buildClang(): Promise<string> {
  if (isWindows) return buildClangWindows();
  if (isMac) return buildClangMac();
  throw new Error(`unsupported platform: ${process.platform}`);
}

// Build the harness with MSVC cl -> out/msvc/djvu_test_msvc.exe.
// cl flags use '-' (a synonym for '/') so Bun's shell doesn't treat them as paths.
async function buildMsvc(): Promise<string> {
  if (!isWindows) throw new Error("MSVC build requires Windows");
  const dir = outDir(false);
  const exe = harnessExeName(false);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const units = cUnits(dir, "obj");
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_msvc.obj`,
    label: "test/bench_ddjvu.cpp",
  };

  const clC = `${DJVUDEC_MSVC_CL_C} -Isrc -Fo${dir}/ -c`;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    const rel = u.src.startsWith(`${ROOT}/`) ? u.src.slice(ROOT.length + 1) : u.src;
    await runCmd(`cl ${clC} ${rel}`, ROOT);
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await runCmd(
      `cl ${MSVC_CL_CXX} ${DJVU_DEFINES} -Fo${bench.obj} -c test/bench_ddjvu.cpp`,
      ROOT,
    );
  }

  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await runCmd(
      `cl -nologo ${objs.join(" ")} ${LIBDJVU} advapi32.lib -Fe:${exePath} -link -LTCG`,
      ROOT,
    );
  }
  return exePath;
}

// Build djvu_test from dist/djvu.c (amalgamation) for -bench. Regenerates dist/
// when src/ is newer. tests.ts uses build() (src/*.c) instead.
async function buildBenchClangWindows(): Promise<string> {
  const dir = outDir(true);
  const exe = harnessExeName(true);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const internalH = `${ROOT}/src/djvu_internal.h`;
  const lib = { src: DIST_C, obj: `${dir}/djvu.o`, label: "dist/djvu.c" };
  const test = {
    src: `${ROOT}/test/djvu_test.c`,
    obj: `${dir}/djvu_test.o`,
    label: "test/djvu_test.c",
  };
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_clang.o`,
    label: "test/bench_ddjvu.cpp",
  };

  if (needsRebuild(lib.obj, lib.src, DIST_H)) {
    await runCmd(`clang ${clangCFlags()} -I${ROOT}/dist -c -o ${lib.obj} ${lib.src}`);
  }
  if (needsRebuild(test.obj, test.src, DIST_H, internalH)) {
    await runCmd(
      `clang ${clangCFlags()} -I${ROOT}/dist -I${ROOT}/src -c -o ${test.obj} ${test.src}`,
    );
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await runCmd(`clang++ ${DJVU_CXXFLAGS_WIN} -c -o ${bench.obj} ${bench.src}`);
  }

  const objs = [lib.obj, test.obj, bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await runCmd(`clang++ ${objs.join(" ")} ${LIBDJVU} -ladvapi32 -o ${exePath}`);
  }
  return exePath;
}

async function buildBenchClangMac(): Promise<string> {
  const dir = outDir(true);
  const exe = harnessExeName(true);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const internalH = `${ROOT}/src/djvu_internal.h`;
  const lib = { src: DIST_C, obj: `${dir}/djvu.o`, label: "dist/djvu.c" };
  const test = {
    src: `${ROOT}/test/djvu_test.c`,
    obj: `${dir}/djvu_test.o`,
    label: "test/djvu_test.c",
  };
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_clang.o`,
    label: "test/bench_ddjvu.cpp",
  };

  if (needsRebuild(lib.obj, lib.src, DIST_H)) {
    await runCmd(`clang ${clangCFlags()} -I${ROOT}/dist -c -o ${lib.obj} ${lib.src}`);
  }
  if (needsRebuild(test.obj, test.src, DIST_H, internalH)) {
    await runCmd(
      `clang ${clangCFlags()} -I${ROOT}/dist -I${ROOT}/src -c -o ${test.obj} ${test.src}`,
    );
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await runCmd(`clang++ ${DJVU_CXXFLAGS_MAC} -c -o ${bench.obj} ${bench.src}`);
  }

  const objs = [lib.obj, test.obj, bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await runCmd(`clang++ ${objs.join(" ")} ${LIBDJVU} ${djvuLinkLibs()} -o ${exePath}`);
  }
  return exePath;
}

async function buildBenchClang(): Promise<string> {
  if (isWindows) return buildBenchClangWindows();
  if (isMac) return buildBenchClangMac();
  throw new Error(`unsupported platform: ${process.platform}`);
}

async function buildBenchMsvc(): Promise<string> {
  if (!isWindows) throw new Error("MSVC build requires Windows");
  const dir = outDir(false);
  const exe = harnessExeName(false);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const internalH = `${ROOT}/src/djvu_internal.h`;
  const lib = { src: DIST_C, obj: `${dir}/djvu.obj`, label: "dist/djvu.c" };
  const test = {
    src: `${ROOT}/test/djvu_test.c`,
    obj: `${dir}/djvu_test.obj`,
    label: "test/djvu_test.c",
  };
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_msvc.obj`,
    label: "test/bench_ddjvu.cpp",
  };

  const clLib = `${DJVUDEC_MSVC_CL_C} -Idist -Fo${dir}/ -c`;
  const clTest = `${DJVUDEC_MSVC_CL_C} -Idist -Isrc -Fo${dir}/ -c`;
  if (needsRebuild(lib.obj, lib.src, DIST_H)) {
    await runCmd(`cl ${clLib} dist/djvu.c`, ROOT);
  }
  if (needsRebuild(test.obj, test.src, DIST_H, internalH)) {
    await runCmd(`cl ${clTest} test/djvu_test.c`, ROOT);
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await runCmd(
      `cl ${MSVC_CL_CXX} ${DJVU_DEFINES} -Fo${bench.obj} -c test/bench_ddjvu.cpp`,
      ROOT,
    );
  }

  const objs = [lib.obj, test.obj, bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await runCmd(
      `cl -nologo ${objs.join(" ")} ${LIBDJVU} advapi32.lib -Fe:${exePath} -link -LTCG`,
      ROOT,
    );
  }
  return exePath;
}

export async function buildBench(useClang = defaultUseClang): Promise<string> {
  await ensureDist();
  await buildLibDjvu();
  const name = harnessExeName(useClang);
  const exePath = `${outDir(useClang)}/${name}`;
  const internalH = `${ROOT}/src/djvu_internal.h`;
  const ext = useClang ? "o" : "obj";
  const libObj = `${outDir(useClang)}/djvu.${ext}`;
  const testObj = `${outDir(useClang)}/djvu_test.${ext}`;
  const benchObj = `${outDir(useClang)}/bench_ddjvu_${useClang ? "clang.o" : "msvc.obj"}`;

  const stale =
    needsRebuild(libObj, DIST_C, DIST_H) ||
    needsRebuild(testObj, `${ROOT}/test/djvu_test.c`, DIST_H, internalH) ||
    needsRebuild(benchObj, `${ROOT}/test/bench_ddjvu.cpp`) ||
    needsRebuild(exePath, LIBDJVU);

  if (!stale && existsSync(exePath)) {
    console.log(`${name} up to date (dist amalgamation)`);
    return exePath;
  }

  console.log(`building ${name} from dist/ (${useClang ? "clang" : "msvc"})...`);
  const exe = useClang ? await buildBenchClang() : await buildBenchMsvc();
  console.log(`built ${name}`);
  return exe;
}

// Build the C library + test harness. djvu_test links the DjVuLibre timing shim
// (bench_ddjvu.cpp) + libdjvu.lib so `djvu_test -bench` can compare decode speed.
// Returns the path to the built executable.
export async function build(useClang = defaultUseClang): Promise<string> {
  if (!useClang && !isWindows) {
    throw new Error("MSVC build requires Windows; use clang on macOS");
  }
  await buildLibDjvu();
  const name = harnessExeName(useClang);
  const exePath = `${outDir(useClang)}/${name}`;
  const stale = needsRebuild(exePath, LIBDJVU); // quick check before logging
  const anyUnit = cUnits(outDir(useClang), useClang ? "o" : "obj").some((u) =>
    needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H),
  );
  const benchObj = `${outDir(useClang)}/bench_ddjvu_${useClang ? "clang.o" : "msvc.obj"}`;
  const benchStale = needsRebuild(benchObj, `${ROOT}/test/bench_ddjvu.cpp`);

  if (!stale && !anyUnit && !benchStale && existsSync(exePath)) {
    console.log(`${name} up to date`);
    return exePath;
  }

  console.log(`building ${name} (${useClang ? "clang" : "msvc"})...`);
  const exe = useClang ? await buildClang() : await buildMsvc();
  console.log(`built ${name}`);
  return exe;
}

// Build a clang + AddressSanitizer harness -> out/clang_asan/. The decoder
// units are instrumented (-fsanitize=address -O1 for readable traces); the
// bench shim + prebuilt libdjvu.lib stay uninstrumented (they're the oracle).
// Used by `bun cmd/tests.ts -asan` to run the verifier under ASan.
const ASAN_DIR = `${OUT_ROOT}/clang_asan`;
const ASAN_EXE = `${ASAN_DIR}/${binName("djvu_test_clang_asan")}`;

async function buildAsanWindows(): Promise<string> {
  await buildLibDjvu();
  mkdirSync(ASAN_DIR, { recursive: true });
  const units = cUnits(ASAN_DIR, "o");
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${ASAN_DIR}/bench_ddjvu_clang.o`,
  };
  const ASAN = "-fsanitize=address";
  let built = false;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    built = true;
    await runCmd(
      `clang ${ASAN} ${clangCFlags("-g -O1")} -I${ROOT}/src -c -o ${u.obj} ${u.src}`,
    );
  }
  if (needsRebuild(bench.obj, bench.src)) {
    built = true;
    await runCmd(`clang++ ${DJVU_CXXFLAGS_WIN} -c -o ${bench.obj} ${bench.src}`);
  }
  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(ASAN_EXE, ...objs, LIBDJVU)) {
    built = true;
    await runCmd(`clang++ ${ASAN} ${objs.join(" ")} ${LIBDJVU} -ladvapi32 -o ${ASAN_EXE}`);
  }
  console.log(built ? `built ${binName("djvu_test_clang_asan")}` : `${binName("djvu_test_clang_asan")} up to date`);
  return ASAN_EXE;
}

async function buildAsanMac(): Promise<string> {
  await buildLibDjvu();
  mkdirSync(ASAN_DIR, { recursive: true });
  const units = cUnits(ASAN_DIR, "o");
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${ASAN_DIR}/bench_ddjvu_clang.o`,
  };
  const ASAN = "-fsanitize=address";
  let built = false;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    built = true;
    await runCmd(
      `clang ${ASAN} ${clangCFlags("-g -O1", false)} -I${ROOT}/src -c -o ${u.obj} ${u.src}`,
    );
  }
  if (needsRebuild(bench.obj, bench.src)) {
    built = true;
    await runCmd(`clang++ ${DJVU_CXXFLAGS_MAC} -c -o ${bench.obj} ${bench.src}`);
  }
  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(ASAN_EXE, ...objs, LIBDJVU)) {
    built = true;
    await runCmd(`clang++ ${ASAN} ${objs.join(" ")} ${LIBDJVU} ${djvuLinkLibs()} -o ${ASAN_EXE}`);
  }
  console.log(built ? `built ${binName("djvu_test_clang_asan")}` : `${binName("djvu_test_clang_asan")} up to date`);
  return ASAN_EXE;
}

export async function buildAsan(): Promise<string> {
  if (isWindows) return buildAsanWindows();
  if (isMac) return buildAsanMac();
  throw new Error(`unsupported platform: ${process.platform}`);
}

if (import.meta.main) {
  await getDeps();
  const args = process.argv.slice(2);
  if (args.includes("-clean")) cleanBuildOutput();
  const useClang = args.includes("-clang") || defaultUseClang;
  if (args.includes("ref")) await buildRef();
  else if (args.includes("asan")) {
    await buildRef();
    await buildAsan();
  } else {
    await buildRef();
    await build(useClang);
  }
}