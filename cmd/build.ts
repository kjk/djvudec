// build.ts -- build driver for the djvu C port (run with `bun cmd/build.ts`).
//
//   bun cmd/build.ts          fetch deps + ref tools + the harness (MSVC default)
//   bun cmd/build.ts -clang   build the harness with clang instead of MSVC
//   bun cmd/build.ts -clean   delete out/ before building (full rebuild)
//   bun cmd/build.ts ref      (re)build the DjVuLibre reference tools
//   bun cmd/build.ts asan     build the clang + AddressSanitizer harness
//
// Three flavours of the djvu_test harness, each in its own directory so they
// never share (or clobber) objects or exes:
//   build()      out/msvc/djvu_test_msvc.exe | out/clang/djvu_test_clang[.exe]
//                src/*.c compiled per file -- the verifier (tests.ts)
//   buildBench() out/msvc_bench/djvu_bench_msvc.exe |
//                out/clang_bench/djvu_bench_clang[.exe]
//                the single-file amalgamation, generated into out/amalg_debug/ (NOT
//                dist/) -- the benchmark (bench.ts)
//   buildAsan()  out/clang_asan/djvu_test_clang_asan[.exe] -- tests.ts -asan
//
// The build is incremental: a source is recompiled only when it (or a shared
// header) is newer than its object, and the exe is relinked whenever any linked
// object or libdjvu is newer than the exe. Each function returns the exe path.
// Verification lives in tests.ts.
import { copyFileSync, existsSync, mkdirSync, rmSync } from "fs";
import { resolve as resolvePath } from "path";
import { ensureAmalgamation } from "./build-dist";
import { DJVULIBRE_DIR, getDeps } from "./get-deps";
import {
  binName,
  clangCFlags,
  defaultUseClang,
  DJVUDEC_MSVC_CL_C,
  headerDeps,
  isMac,
  isWindows,
  LIB_HEADERS,
  LIB_SRCS,
  MSVC_CL_CXX,
  MSVC_LINK,
  msvcFd,
  needsRebuild,
  objBase,
  ROOT,
  runCmd,
} from "./util";

const DJVULIBRE = DJVULIBRE_DIR.replaceAll("\\", "/"); // deps/ checkout (see get-deps.ts)
const OUT_ROOT = `${ROOT}/out`;
const REF = `${ROOT}/ref_build`;
const OBJDIR = `${REF}/djvuobj`;

// Windows: libdjvu.lib + llvm-lib; Unix: libdjvu.a + ar.
const LIBDJVU = `${REF}/${isWindows ? "libdjvu.lib" : "libdjvu.a"}`;

// DjVuLibre include + define flags (oracle only — NOT our code).
// _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH: the MSVC STL refuses (STL1000) a
// clang older than the one its VS release ships; the clang on PATH on CI
// runners can lag the STL by a version.
const DJVU_DEFINES_WIN =
  `-DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

// macOS + Linux: same AUTOCONF/UNIX defines DjVuLibre expects without config.h.
const DJVU_DEFINES_UNIX =
  `-DAUTOCONF -DHAVE_STDINCLUDES ` +
  `-DHAVE_NAMESPACES -DHAVE_PTHREAD -DHAVE_STDINT_H -DHAVE_WCHAR_H ` +
  `-DHAVE_STRERROR -DHAVE_DIRENT_H -DHAVE_SYS_TIME_H ` +
  `-DHAS_WCHAR=1 -DHAS_WCTYPE=1 -DHAS_MBSTATE=1 -DUNIX ` +
  `-DDIR_DATADIR='"/usr/share"' ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

// clang++ compile flags for DjVuLibre and the bench shim (keep -w, no -Werror).
// -O3: both sides are built at max optimization so `-bench` is a fair compare.
const DJVU_CXXFLAGS =
  `-std=c++14 -w -O3 ${isWindows ? DJVU_DEFINES_WIN : DJVU_DEFINES_UNIX}`;

// Windows: advapi32. Unix: pthread; Linux also needs libm (macOS libSystem
// already provides it).
const DJVU_LINK_LIBS = isWindows
  ? "-ladvapi32"
  : isMac
    ? "-lpthread"
    : "-lpthread -lm";

/** Delete ALL of out/ (every toolchain, bench snapshots, fuzz + prof builds). */
export function cleanBuildOutput(): void {
  rmSync(OUT_ROOT, { recursive: true, force: true });
}

export function refToolPath(tool: string): string {
  return `${REF}/${binName(tool)}`;
}

// Build the DjVuLibre reference tools (static, decode oracle): ddjvu, djvutxt,
// bzz, djvused from tools/, plus our per-layer probes from test/. Each is built
// once and then left alone (delete ref_build/ to force a rebuild).
export async function buildRef(): Promise<void> {
  mkdirSync(REF, { recursive: true });
  const libsrc = `${DJVULIBRE}/libdjvu/*.cpp`;
  const tools = [
    ...["ddjvu", "djvutxt", "bzz", "djvused"].map((t) => ({
      name: t,
      src: `${DJVULIBRE}/tools/${t}.cpp`,
    })),
    ...["iw44ref", "jb2ref", "jb2prof"].map((t) => ({
      name: t,
      src: `${ROOT}/test/${t}.cpp`,
    })),
  ];
  for (const { name, src } of tools) {
    const exe = refToolPath(name);
    if (existsSync(exe)) continue;
    await runCmd(
      `clang++ ${DJVU_CXXFLAGS} ${libsrc} ${src} ${DJVU_LINK_LIBS} -o ${exe}`,
    );
  }
  console.log("ref tools ready");
}

// Compile all of libdjvu into a cached static lib (one-time, slow) so djvu_test
// can link DjVuLibre's decoder for `-bench` without recompiling it every build.
export async function buildLibDjvu(): Promise<void> {
  if (existsSync(LIBDJVU)) return;
  const name = isWindows ? "libdjvu.lib" : "libdjvu.a";
  console.log(`building ${name} (one-time, slow)...`);
  mkdirSync(OBJDIR, { recursive: true });
  await runCmd(`clang++ ${DJVU_CXXFLAGS} -c ${DJVULIBRE}/libdjvu/*.cpp`, OBJDIR);
  await runCmd(
    isWindows ? `llvm-lib /out:${LIBDJVU} *.o` : `ar rcs ${LIBDJVU} *.o`,
    OBJDIR,
  );
  console.log(`built ${name}`);
}

// ---------------------------------------------------------------------------
// djvu_test harness (test/djvu_test.c + the decoder + the DjVuLibre timing shim
// test/bench_ddjvu.cpp + libdjvu), in three flavours -- see the header comment.

type Flavour = "test" | "bench" | "asan";

// One incremental build step: run cmd when out is missing or older than an input.
type Step = { out: string; inputs: string[]; cmd: string; cwd?: string };

const stepStale = (s: Step) => needsRebuild(s.out, ...s.inputs);

type Harness = { name: string; dir: string; exe: string; steps: Step[] };

// Debug amalgamation for the bench harness (see harnessPlan): its own dir so a
// shipped-style amalgamation left in out/amalg by an older build is never reused.
const AMALG_DIR = `${OUT_ROOT}/amalg_debug`;
const TEST_C = `${ROOT}/test/djvu_test.c`;
const SHIM_CPP = `${ROOT}/test/bench_ddjvu.cpp`;

/** Path relative to the repo root (cl runs with cwd=ROOT and relative paths). */
const rootRel = (p: string) =>
  p.startsWith(`${ROOT}/`) ? p.slice(ROOT.length + 1) : p;

function harnessDir(flavour: Flavour, useClang: boolean): string {
  if (flavour === "asan") return `${OUT_ROOT}/clang_asan`;
  const tc = useClang ? "clang" : "msvc";
  return `${OUT_ROOT}/${flavour === "bench" ? `${tc}_bench` : tc}`;
}

function harnessName(flavour: Flavour, useClang: boolean): string {
  if (flavour === "asan") return binName("djvu_test_clang_asan");
  const tc = useClang ? "clang" : "msvc";
  return binName(`${flavour === "bench" ? "djvu_bench" : "djvu_test"}_${tc}`);
}

// The compile + link steps for one harness flavour. The last step is the link;
// its inputs are every linked object plus libdjvu, so the exe is stale whenever
// any of them is newer (e.g. after a link that failed because the exe was
// locked, or after an object was recompiled by an interrupted earlier run).
function harnessPlan(flavour: Flavour, useClang: boolean): Harness {
  const dir = harnessDir(flavour, useClang);
  const name = harnessName(flavour, useClang);
  const exe = `${dir}/${name}`;
  const ext = useClang ? "o" : "obj";
  const steps: Step[] = [];

  // clang: per-file -c -o. ASan: -O1 for readable traces.
  const san = flavour === "asan" ? "-fsanitize=address " : "";
  const cflags = `${san}${clangCFlags(flavour === "asan" ? "-g -O1" : "-g -O3")}`;
  // cl: flags use '-' (a synonym for '/') and repo-relative paths with
  // cwd=ROOT, so Bun's shell doesn't mangle them.
  const compileC = (src: string, obj: string, incs: string[], inputs: string[]) => {
    if (useClang) {
      steps.push({
        out: obj,
        inputs: [src, ...inputs],
        cmd: `clang ${cflags} ${incs.map((i) => `-I${i}`).join(" ")} -c -o ${obj} ${src}`,
      });
    } else {
      const inc = incs.map((i) => `-I${rootRel(i)}`).join(" ");
      steps.push({
        out: obj,
        inputs: [src, ...inputs],
        cmd: `cl ${DJVUDEC_MSVC_CL_C} ${inc} -Fo${dir}/ ${msvcFd(dir)} -c ${rootRel(src)}`,
        cwd: ROOT,
      });
    }
  };

  // The decoder: src/*.c one object each, or the amalgamation as one object.
  // djvu_test.c also includes djvu_internal.h (debug + BZZ hooks), from src/.
  // The bench build uses the *debug* amalgamation (out/amalg_debug: all modules
  // incl. src/debug.c, DJVU_DEBUG_ONLY regions kept), because djvu_test calls
  // the djvu_debug_* hooks; the shipped dist/djvu.c has none of that.
  if (flavour === "bench") {
    const a = { h: `${AMALG_DIR}/djvu.h`, c: `${AMALG_DIR}/djvu.c` };
    compileC(a.c, `${dir}/djvu.${ext}`, [AMALG_DIR], [a.h]);
    compileC(TEST_C, `${dir}/djvu_test.${ext}`, [AMALG_DIR, `${ROOT}/src`], [a.h, ...headerDeps(TEST_C)]);
  } else {
    for (const s of LIB_SRCS) {
      compileC(`${ROOT}/${s}`, `${dir}/${objBase(s)}.${ext}`, [`${ROOT}/src`], LIB_HEADERS);
    }
    compileC(TEST_C, `${dir}/djvu_test.${ext}`, [`${ROOT}/src`], headerDeps(TEST_C));
  }

  // The DjVuLibre timing shim (C++, uninstrumented even under ASan: with the
  // prebuilt libdjvu it is the oracle, not the code under test).
  const shim = `${dir}/bench_ddjvu_${useClang ? "clang.o" : "msvc.obj"}`;
  steps.push(
    useClang
      ? {
          out: shim,
          inputs: [SHIM_CPP],
          cmd: `clang++ ${DJVU_CXXFLAGS} -c -o ${shim} ${SHIM_CPP}`,
        }
      : {
          out: shim,
          inputs: [SHIM_CPP],
          cmd: `cl ${MSVC_CL_CXX} ${DJVU_DEFINES_WIN} -Fo${shim} ${msvcFd(dir)} -c ${rootRel(SHIM_CPP)}`,
          cwd: ROOT,
        },
  );

  // libdjvu.lib uses the static CRT (/MT); MSVC_CL_COMMON has -MT to match.
  const objs = steps.map((s) => s.out);
  steps.push(
    useClang
      ? {
          out: exe,
          inputs: [...objs, LIBDJVU],
          cmd: `clang++ ${san}${objs.join(" ")} ${LIBDJVU} ${DJVU_LINK_LIBS} -o ${exe}`,
        }
      : {
          out: exe,
          inputs: [...objs, LIBDJVU],
          cmd: `cl -nologo ${objs.join(" ")} ${LIBDJVU} advapi32.lib -Fe:${exe} -link ${MSVC_LINK}`,
          cwd: ROOT,
        },
  );
  return { name, dir, exe, steps };
}

async function buildHarness(flavour: Flavour, useClang: boolean): Promise<string> {
  if (!useClang && !isWindows) {
    throw new Error("MSVC build requires Windows; use clang on Unix");
  }
  await buildLibDjvu();
  if (flavour === "bench") ensureAmalgamation(AMALG_DIR, { debug: true });
  const h = harnessPlan(flavour, useClang);
  mkdirSync(h.dir, { recursive: true });

  if (!h.steps.some(stepStale)) {
    console.log(`${h.name} up to date`);
  } else {
    const what = flavour === "asan" ? "clang+asan" : useClang ? "clang" : "msvc";
    const from = flavour === "bench" ? ", out/amalg_debug amalgamation" : "";
    console.log(`building ${h.name} (${what}${from})...`);
    // Re-check each step as we reach it: a recompiled object makes the link stale.
    for (const s of h.steps) {
      if (stepStale(s)) await runCmd(s.cmd, s.cwd);
    }
    console.log(`built ${h.name}`);
  }
  if (flavour === "asan" && isWindows) await copyAsanRuntimeDll(h.dir);
  return h.exe;
}

// Build the C library (src/*.c) + test harness. djvu_test links the DjVuLibre
// timing shim (bench_ddjvu.cpp) + libdjvu so the verifier has its in-process
// oracle. Returns the path to the built executable.
export function build(useClang = defaultUseClang): Promise<string> {
  return buildHarness("test", useClang);
}

// Build the same harness from the single-file amalgamation, for bench.ts
// (`-bench` / `-bench-sum`): the amalgamation is what consumers ship, and one
// translation unit is what the optimizer sees there. The amalgamation is
// generated into out/amalg_debug/ by the same code as build-dist.ts and refreshed
// when src/ is newer; the tracked dist/ files are never read or written, and
// no WASM build is triggered.
export function buildBench(useClang = defaultUseClang): Promise<string> {
  return buildHarness("bench", useClang);
}

/** Remove only the bench build (out/amalg_debug + this toolchain's bench dir). */
export function cleanBenchOutput(useClang = defaultUseClang): void {
  rmSync(AMALG_DIR, { recursive: true, force: true });
  rmSync(harnessDir("bench", useClang), { recursive: true, force: true });
}

// Build a clang + AddressSanitizer harness -> out/clang_asan/. The decoder
// units are instrumented (-fsanitize=address -O1 for readable traces); the
// bench shim + prebuilt libdjvu stay uninstrumented (they're the oracle).
// Used by `bun cmd/tests.ts -asan` to run the verifier under ASan.
export function buildAsan(): Promise<string> {
  return buildHarness("asan", true);
}

// DjVuLibre oracle for the differential fuzz target (test/fuzz_diff.c):
// compiles the in-memory ddjvuapi shim test/fuzz_ddjvu.cpp UNinstrumented
// (it is the oracle, like bench_ddjvu.cpp in the ASan harness) and ensures
// the prebuilt libdjvu. Returns what the fuzz link needs.
export async function buildFuzzOracle(
  dir: string,
): Promise<{ objs: string[]; linkLibs: string }> {
  await buildLibDjvu();
  const src = `${ROOT}/test/fuzz_ddjvu.cpp`;
  const obj = `${dir}/fuzz_ddjvu.o`;
  if (needsRebuild(obj, src)) {
    await runCmd(`clang++ ${DJVU_CXXFLAGS} -c -o ${obj} ${src}`);
  }
  return { objs: [obj, LIBDJVU], linkLibs: DJVU_LINK_LIBS };
}

// The exe links the DYNAMIC asan runtime (clang_rt.asan_dynamic-x86_64.dll),
// which lives in clang's resource dir (lib/clang/<ver>/lib/windows), not on
// PATH; without a copy next to the exe the loader fails (bash exit 127 /
// STATUS_DLL_NOT_FOUND) before main.
export async function copyAsanRuntimeDll(dir: string): Promise<void> {
  const dllName = "clang_rt.asan_dynamic-x86_64.dll";
  // Normalize: CopyFileW fails ENOENT on a path with a mixed-slash ".."
  // segment (e.g. ".../cmd/../out/fuzz/...dll"), even when it resolves.
  const dst = resolvePath(dir, dllName);
  if (existsSync(dst)) return;
  const proc = Bun.spawnSync(["clang", "-print-resource-dir"]);
  const resDir = proc.stdout.toString().trim();
  const src = resolvePath(resDir, "lib/windows", dllName);
  if (!existsSync(src)) {
    console.warn(`warning: ${src} not found; ${dllName} must be on PATH`);
    return;
  }
  copyFileSync(src, dst);
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
