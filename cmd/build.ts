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
const LIBDJVU = `${REF}/libdjvu.lib`; // cached static lib (for djvu_test -bench)
const OBJDIR = `${REF}/djvuobj`;

const outDir = (useClang: boolean) =>
  `${OUT_ROOT}/${useClang ? "clang" : "msvc"}`;

// DjVuLibre C++ compile flags (static link, no dll import/export indirection).
// -O3: both sides are built at max optimization so `-bench` is a fair compare.
const DJVU_CXXFLAGS =
  `-std=c++14 -w -O3 -DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

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

export function cleanBuildOutput(): void {
  rmSync(OUT_ROOT, { recursive: true, force: true });
}

// Build ddjvu.exe / djvutxt.exe from DjVuLibre (static, decode oracle).
export async function buildRef() {
  mkdirSync(REF, { recursive: true });
  const common = DJVU_CXXFLAGS;
  const libsrc = `${DJVULIBRE}/libdjvu/*.cpp`;
  for (const tool of ["ddjvu", "djvutxt", "bzz", "djvused"]) {
    const exe = `${REF}/${tool}.exe`;
    if (existsSync(exe)) continue;
    console.log(`building ref tool ${tool}...`);
    await $`clang++ ${{ raw: common }} ${{ raw: libsrc }} ${DJVULIBRE}/tools/${tool}.cpp -ladvapi32 -o ${exe}`;
  }
  // iw44ref: decodes a FORM:PM44 directly via IW44Image (no gamma pipeline),
  // used to verify the IW44 codec in isolation. Source lives in test/.
  if (!existsSync(`${REF}/iw44ref.exe`)) {
    console.log("building ref tool iw44ref...");
    await $`clang++ ${{ raw: common }} ${{ raw: libsrc }} ${ROOT}/test/iw44ref.cpp -ladvapi32 -o ${REF}/iw44ref.exe`;
  }
  // jb2ref: decodes a raw Sjbz with DjVuLibre's JB2Image (dumps blits / mask),
  // used to verify the JB2 codec in isolation.
  if (!existsSync(`${REF}/jb2ref.exe`)) {
    console.log("building ref tool jb2ref...");
    await $`clang++ ${{ raw: common }} ${{ raw: libsrc }} ${ROOT}/test/jb2ref.cpp -ladvapi32 -o ${REF}/jb2ref.exe`;
  }
  console.log("ref tools ready");
}

// Compile all of libdjvu into a cached static lib (one-time, slow) so djvu_test
// can link DjVuLibre's decoder for `-bench` without recompiling it every build.
export async function buildLibDjvu() {
  if (existsSync(LIBDJVU)) return;
  console.log("building libdjvu.lib (one-time, slow)...");
  mkdirSync(OBJDIR, { recursive: true });
  await $`clang++ ${{ raw: DJVU_CXXFLAGS }} -c ${{ raw: `${DJVULIBRE}/libdjvu/*.cpp` }}`.cwd(OBJDIR);
  await $`llvm-lib /out:${LIBDJVU} ${{ raw: "*.o" }}`.cwd(OBJDIR);
  console.log("built libdjvu.lib");
}

// DjVuLibre include + define flags shared by the clang and cl shim builds.
const DJVU_DEFINES =
  `-DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

// On Windows the default toolchain is MSVC; -clang forces clang. Elsewhere only
// clang is available.
export const defaultUseClang = process.platform !== "win32";

// MSVC cl.exe flags (use '-' not '/' — Bun's shell treats backslashes as escapes).
// -Ob3: inline any suitable function (aggressive inlining with /O2).
export const MSVC_CL_COMMON = `-nologo -O2 -Ob3 -GL -MT`;
export const MSVC_CL_C = `${MSVC_CL_COMMON} -W3 -std:c11 -D_CRT_SECURE_NO_WARNINGS`;
export const MSVC_CL_CXX = `${MSVC_CL_COMMON} -EHsc -std:c++14`;
const exeName = (useClang: boolean) =>
  `djvu_test_${useClang ? "clang" : "msvc"}.exe`;

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

// Build the harness with clang -> out/clang/djvu_test_clang.exe.
async function buildClang(): Promise<string> {
  const dir = outDir(true);
  const exe = exeName(true);
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
    await $`clang -std=c11 -g -O3 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I${ROOT}/src -c -o ${u.obj} ${u.src}`;
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await $`clang++ ${{ raw: DJVU_CXXFLAGS }} -c -o ${bench.obj} ${bench.src}`;
  }

  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await $`clang++ ${{ raw: objs.join(" ") }} ${LIBDJVU} -ladvapi32 -o ${exePath}`;
  }
  return exePath;
}

// Build the harness with MSVC cl -> out/msvc/djvu_test_msvc.exe.
// cl flags use '-' (a synonym for '/') so Bun's shell doesn't treat them as paths.
async function buildMsvc(): Promise<string> {
  const dir = outDir(false);
  const exe = exeName(false);
  const exePath = `${dir}/${exe}`;
  mkdirSync(dir, { recursive: true });

  const units = cUnits(dir, "obj");
  const bench = {
    src: `${ROOT}/test/bench_ddjvu.cpp`,
    obj: `${dir}/bench_ddjvu_msvc.obj`,
    label: "test/bench_ddjvu.cpp",
  };

  const clC = `${MSVC_CL_C} -Isrc -Fo${dir}/ -c`;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    const rel = u.src.startsWith(`${ROOT}/`) ? u.src.slice(ROOT.length + 1) : u.src;
    await $`cl ${{ raw: clC }} ${{ raw: rel }}`.cwd(ROOT);
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await $`cl ${{ raw: MSVC_CL_CXX }} ${{ raw: DJVU_DEFINES }} -Fo${bench.obj} -c test/bench_ddjvu.cpp`.cwd(ROOT);
  }

  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await $`cl -nologo ${{ raw: objs.join(" ") }} ${LIBDJVU} advapi32.lib -Fe:${exePath} -link -LTCG`.cwd(ROOT);
  }
  return exePath;
}

// Build djvu_test from dist/djvu.c (amalgamation) for -bench. Regenerates dist/
// when src/ is newer. tests.ts uses build() (src/*.c) instead.
async function buildBenchClang(): Promise<string> {
  const dir = outDir(true);
  const exe = exeName(true);
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
    await $`clang -std=c11 -g -O3 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I${ROOT}/dist -c -o ${lib.obj} ${lib.src}`;
  }
  if (needsRebuild(test.obj, test.src, DIST_H, internalH)) {
    await $`clang -std=c11 -g -O3 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I${ROOT}/dist -I${ROOT}/src -c -o ${test.obj} ${test.src}`;
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await $`clang++ ${{ raw: DJVU_CXXFLAGS }} -c -o ${bench.obj} ${bench.src}`;
  }

  const objs = [lib.obj, test.obj, bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await $`clang++ ${{ raw: objs.join(" ") }} ${LIBDJVU} -ladvapi32 -o ${exePath}`;
  }
  return exePath;
}

async function buildBenchMsvc(): Promise<string> {
  const dir = outDir(false);
  const exe = exeName(false);
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

  const clLib = `${MSVC_CL_C} -Idist -Fo${dir}/ -c`;
  const clTest = `${MSVC_CL_C} -Idist -Isrc -Fo${dir}/ -c`;
  if (needsRebuild(lib.obj, lib.src, DIST_H)) {
    await $`cl ${{ raw: clLib }} dist/djvu.c`.cwd(ROOT);
  }
  if (needsRebuild(test.obj, test.src, DIST_H, internalH)) {
    await $`cl ${{ raw: clTest }} test/djvu_test.c`.cwd(ROOT);
  }
  if (needsRebuild(bench.obj, bench.src)) {
    await $`cl ${{ raw: MSVC_CL_CXX }} ${{ raw: DJVU_DEFINES }} -Fo${bench.obj} -c test/bench_ddjvu.cpp`.cwd(ROOT);
  }

  const objs = [lib.obj, test.obj, bench.obj];
  if (needsRebuild(exePath, ...objs, LIBDJVU)) {
    await $`cl -nologo ${{ raw: objs.join(" ") }} ${LIBDJVU} advapi32.lib -Fe:${exePath} -link -LTCG`.cwd(ROOT);
  }
  return exePath;
}

export async function buildBench(useClang = defaultUseClang): Promise<string> {
  await ensureDist();
  await buildLibDjvu();
  const name = exeName(useClang);
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
  await buildLibDjvu();
  const name = exeName(useClang);
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
const ASAN_EXE = `${ASAN_DIR}/djvu_test_clang_asan.exe`;

export async function buildAsan(): Promise<string> {
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
    await $`clang ${{ raw: ASAN }} -std=c11 -g -O1 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I${ROOT}/src -c -o ${u.obj} ${u.src}`;
  }
  if (needsRebuild(bench.obj, bench.src)) {
    built = true;
    await $`clang++ ${{ raw: DJVU_CXXFLAGS }} -c -o ${bench.obj} ${bench.src}`;
  }
  const objs = [...units.map((u) => u.obj), bench.obj];
  if (needsRebuild(ASAN_EXE, ...objs, LIBDJVU)) {
    built = true;
    await $`clang++ ${{ raw: ASAN }} ${{ raw: objs.join(" ") }} ${LIBDJVU} -ladvapi32 -o ${ASAN_EXE}`;
  }
  console.log(built ? "built djvu_test_clang_asan.exe" : "djvu_test_clang_asan.exe up to date");
  return ASAN_EXE;
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