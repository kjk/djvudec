// compare-code-sizes.ts -- compare stripped release binary sizes: djvudec vs libdjvu.
//
// Builds two minimal probe programs that exercise the same viewer-facing APIs
// (page render, hidden text, hyperlinks, outline), links each against its
// decoder with matching release optimization, then reports executable and .text
// sizes.
//
//   bun cmd/compare-code-sizes.ts [file.djvu] [-clang] [-clean]
//
// With no file, uses the first corpus .djvu (deps/ checkouts).
import { $ } from "bun";
import { existsSync, mkdirSync, readdirSync, rmSync, statSync } from "fs";
import { join } from "path";
import {
  defaultUseClang,
  isMac,
  isWindows,
  MSVC_CL_CXX,
  MSVC_LINK,
  msvcFd,
} from "./build";
import { DJVULIBRE_DIR, getDeps } from "./get-deps";
import { LIB_SRCS } from "./build-lib";
import { corpusFiles } from "./corpus";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const DJVULIBRE = DJVULIBRE_DIR.replaceAll("\\", "/");
const OUT = `${ROOT}/out/code_size`;

const DJVU_SRCS = LIB_SRCS.filter((s) => s !== "src/debug.c");

const PROBE_DJVUDEC = `${ROOT}/test/size_probe_djvudec.c`;
const PROBE_LIBDJVU = `${ROOT}/test/size_probe_libdjvu.cpp`;
const PUBLIC_H = `${ROOT}/src/djvu.h`;

const RELEASE_CLANG_C =
  "-std=c11 -O3 -DNDEBUG -ffunction-sections -fdata-sections " +
  "-fno-asynchronous-unwind-tables -D_CRT_SECURE_NO_WARNINGS";
const RELEASE_CLANG_CXX_WIN =
  "-std=c++14 -w -O3 -DNDEBUG -ffunction-sections -fdata-sections " +
  "-fno-asynchronous-unwind-tables -DHAVE_NAMESPACES -DWIN32 " +
  "-D_CRT_SECURE_NO_WARNINGS -DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT " +
  "-DMINILISPAPI_EXPORT " +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;
const RELEASE_CLANG_CXX_MAC =
  "-std=c++14 -w -O3 -DNDEBUG -ffunction-sections -fdata-sections " +
  "-fno-asynchronous-unwind-tables -DAUTOCONF -DHAVE_STDINCLUDES " +
  "-DHAVE_NAMESPACES -DHAVE_PTHREAD -DHAVE_STDINT_H -DHAVE_WCHAR_H " +
  "-DHAVE_STRERROR -DHAVE_DIRENT_H -DHAVE_SYS_TIME_H " +
  "-DHAS_WCHAR=1 -DHAS_WCTYPE=1 -DHAS_MBSTATE=1 -DUNIX " +
  `-DDIR_DATADIR='"/usr/share"' ` +
  "-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT " +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;
const RELEASE_CLANG_LINK_GNU = "-Wl,--gc-sections -Wl,-s";
const RELEASE_CLANG_LINK_MAC = "-Wl,-dead_strip";
const RELEASE_CLANG_LINK_WIN = "-Wl,/OPT:REF -Wl,/OPT:ICF";
function releaseLibPath(dir: string): string {
  return `${dir}/libdjvu_release${isWindows ? ".lib" : ".a"}`;
}
const RELEASE_MSVC_C =
  "-nologo -O2 -Ob3 -GL -Zi -MT -std:c11 -DNDEBUG -D_CRT_SECURE_NO_WARNINGS";
const RELEASE_MSVC_CXX = `${MSVC_CL_CXX} -DNDEBUG`;
const RELEASE_MSVC_LINK = MSVC_LINK;
const DJVU_DEFINES =
  `-DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

function binName(base: string): string {
  return isWindows ? `${base}.exe` : base;
}

function objBase(src: string): string {
  return src.replace(/^src\//, "").replace(/\.c$/, "");
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

async function runCmd(cmd: string, cwd?: string): Promise<void> {
  console.log(cwd ? `+ cd ${cwd} && ${cmd}` : `+ ${cmd}`);
  const shell = $`${{ raw: cmd }}`;
  if (cwd) await shell.cwd(cwd);
  else await shell;
}

function formatBytes(n: number): string {
  if (n >= 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(2)} MiB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${n} B`;
}

function formatDelta(a: number, b: number): string {
  const d = a - b;
  const pct = b !== 0 ? (100 * d) / b : 0;
  const sign = d >= 0 ? "+" : "";
  return `${sign}${formatBytes(d)} (${sign}${pct.toFixed(1)}%)`;
}

type SectionSizes = {
  text: number | null;
  rdata: number | null;
  data: number | null;
  bss: number | null;
  total: number | null;
};

function parseElfSectionSizes(text: string): SectionSizes {
  const sizes: SectionSizes = {
    text: null,
    rdata: null,
    data: null,
    bss: null,
    total: null,
  };
  for (const line of text.split(/\r?\n/)) {
    const sec = line.match(/^\s*\.(text|rdata|data|bss)\s+(\d+)/);
    if (sec) {
      const n = Number.parseInt(sec[2]!, 10);
      if (sec[1] === "text") sizes.text = n;
      else if (sec[1] === "rdata") sizes.rdata = n;
      else if (sec[1] === "data") sizes.data = n;
      else if (sec[1] === "bss") sizes.bss = n;
      continue;
    }
    const tot = line.match(/^\s*Total\s+(\d+)/);
    if (tot) sizes.total = Number.parseInt(tot[1]!, 10);
  }
  return sizes;
}

function parseMachOSectionSizes(text: string): SectionSizes {
  const sizes: SectionSizes = {
    text: 0,
    rdata: 0,
    data: 0,
    bss: 0,
    total: null,
  };
  for (const line of text.split(/\r?\n/)) {
    const sec = line.match(
      /Section (__text|__const|__cstring|__data|__bss|__common):\s+(\d+)/,
    );
    if (!sec) continue;
    const n = Number.parseInt(sec[2]!, 10);
    switch (sec[1]) {
      case "__text":
        sizes.text! += n;
        break;
      case "__const":
      case "__cstring":
        sizes.rdata! += n;
        break;
      case "__data":
        sizes.data! += n;
        break;
      case "__bss":
      case "__common":
        sizes.bss! += n;
        break;
    }
  }
  sizes.total =
    sizes.text! + sizes.rdata! + sizes.data! + sizes.bss!;
  return sizes;
}

async function readSectionSizes(exe: string): Promise<SectionSizes> {
  const empty: SectionSizes = {
    text: null,
    rdata: null,
    data: null,
    bss: null,
    total: null,
  };
  try {
    if (isMac) {
      const out = await $`size -m ${{ raw: exe }}`.quiet().nothrow();
      if (out.exitCode !== 0) return empty;
      return parseMachOSectionSizes(out.text());
    }
    const out = await $`llvm-size -A ${{ raw: exe }}`.quiet().nothrow();
    if (out.exitCode !== 0) return empty;
    return parseElfSectionSizes(out.text());
  } catch {
    return empty;
  }
}

function clangLinkFlags(): string {
  if (isWindows) return RELEASE_CLANG_LINK_WIN;
  if (isMac) return RELEASE_CLANG_LINK_MAC;
  return RELEASE_CLANG_LINK_GNU;
}

function stripNote(useClang: boolean): string {
  if (!useClang) return "LTCG";
  if (isWindows) return "OPT:REF/ICF + llvm-strip";
  if (isMac) return "-dead_strip + strip";
  return "--gc-sections -s";
}

async function stripExe(exe: string): Promise<void> {
  if (!existsSync(exe)) return;
  if (isMac) {
    await $`strip ${{ raw: exe }}`.quiet().nothrow();
    return;
  }
  const llvm = await $`llvm-strip ${{ raw: exe }}`.quiet().nothrow();
  if (llvm.exitCode !== 0) await $`strip ${{ raw: exe }}`.quiet().nothrow();
}

async function buildReleaseLibDjvu(dir: string, useClang: boolean): Promise<string> {
  const lib = releaseLibPath(dir);
  const objdir = `${dir}/libdjvuobj`;
  const libsrc = `${DJVULIBRE}/libdjvu/*.cpp`;
  mkdirSync(objdir, { recursive: true });

  const stamp = `${objdir}/.stamp`;
  if (!needsRebuild(lib, stamp)) return lib;

  console.log("building release libdjvu (one-time, slow)...");
  if (useClang) {
    const cxx = isMac ? RELEASE_CLANG_CXX_MAC : RELEASE_CLANG_CXX_WIN;
    await runCmd(`clang++ ${cxx} -c ${libsrc}`, objdir);
    if (isWindows) {
      await runCmd(`llvm-lib /out:${lib} *.o`, objdir);
    } else {
      await runCmd(`ar rcs ${lib} *.o`, objdir);
    }
  } else {
    await runCmd(
      `cl ${RELEASE_MSVC_CXX} ${DJVU_DEFINES} -Fo${objdir}/ ${msvcFd(objdir)} -c libdjvu/*.cpp`,
      DJVULIBRE,
    );
    await runCmd(`llvm-lib /out:${lib} ${objdir}/*.obj`, ROOT);
  }
  await Bun.write(stamp, "");
  console.log(`built ${lib}`);
  return lib;
}

async function buildDjvudecProbeClang(dir: string): Promise<string> {
  const exe = `${dir}/${binName("size_probe_djvudec")}`;
  mkdirSync(dir, { recursive: true });

  const units = [
    ...DJVU_SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${dir}/${objBase(s)}.o`,
    })),
    {
      src: PROBE_DJVUDEC,
      obj: `${dir}/size_probe_djvudec.o`,
    },
  ];

  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, PUBLIC_H)) continue;
    await runCmd(
      `clang ${RELEASE_CLANG_C} -I${ROOT}/src -c -o ${u.obj} ${u.src}`,
    );
  }

  const objs = units.map((u) => u.obj);
  if (needsRebuild(exe, ...objs)) {
    await runCmd(`clang ${objs.join(" ")} ${clangLinkFlags()} -o ${exe}`);
    await stripExe(exe);
  }
  return exe;
}

async function buildDjvudecProbeMsvc(dir: string): Promise<string> {
  const exe = `${dir}/${binName("size_probe_djvudec")}`;
  mkdirSync(dir, { recursive: true });

  const units = [
    ...DJVU_SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${dir}/${objBase(s)}.obj`,
      rel: s,
    })),
    {
      src: PROBE_DJVUDEC,
      obj: `${dir}/size_probe_djvudec.obj`,
      rel: "test/size_probe_djvudec.c",
    },
  ];

  const clC = `${RELEASE_MSVC_C} -Isrc -Fo${dir}/ ${msvcFd(dir)} -c`;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, PUBLIC_H)) continue;
    await runCmd(`cl ${clC} ${u.rel}`, ROOT);
  }

  const objs = units.map((u) => u.obj);
  if (needsRebuild(exe, ...objs)) {
    await runCmd(
      `cl -nologo ${objs.join(" ")} -Fe:${exe} -link ${RELEASE_MSVC_LINK}`,
      ROOT,
    );
  }
  return exe;
}

async function buildLibdjvuProbeClang(dir: string): Promise<string> {
  const exe = `${dir}/${binName("size_probe_libdjvu")}`;
  const obj = `${dir}/size_probe_libdjvu.o`;
  mkdirSync(dir, { recursive: true });
  const cxx = isMac ? RELEASE_CLANG_CXX_MAC : RELEASE_CLANG_CXX_WIN;
  const link = isMac ? "-lpthread" : "-ladvapi32";

  const lib = await buildReleaseLibDjvu(dir, true);

  if (needsRebuild(obj, PROBE_LIBDJVU)) {
    await runCmd(`clang++ ${cxx} -c -o ${obj} ${PROBE_LIBDJVU}`);
  }
  if (needsRebuild(exe, obj, lib)) {
    await runCmd(
      `clang++ ${obj} ${lib} ${link} ${clangLinkFlags()} -o ${exe}`,
    );
    await stripExe(exe);
  }
  return exe;
}

async function buildLibdjvuProbeMsvc(dir: string): Promise<string> {
  const exe = `${dir}/${binName("size_probe_libdjvu")}`;
  const obj = `${dir}/size_probe_libdjvu.obj`;
  mkdirSync(dir, { recursive: true });

  const lib = await buildReleaseLibDjvu(dir, false);

  if (needsRebuild(obj, PROBE_LIBDJVU)) {
    await runCmd(
      `cl ${RELEASE_MSVC_CXX} ${DJVU_DEFINES} -Fo${obj} ${msvcFd(dir)} -c test/size_probe_libdjvu.cpp`,
      ROOT,
    );
  }
  if (needsRebuild(exe, obj, lib)) {
    await runCmd(
      `cl -nologo ${obj} ${lib} advapi32.lib -Fe:${exe} -link ${RELEASE_MSVC_LINK}`,
      ROOT,
    );
  }
  return exe;
}

async function buildProbes(useClang: boolean): Promise<{ djvudec: string; libdjvu: string }> {
  const dir = `${OUT}/${useClang ? "clang" : "msvc"}`;
  if (useClang) {
    return {
      djvudec: await buildDjvudecProbeClang(dir),
      libdjvu: await buildLibdjvuProbeClang(dir),
    };
  }
  return {
    djvudec: await buildDjvudecProbeMsvc(dir),
    libdjvu: await buildLibdjvuProbeMsvc(dir),
  };
}

function printRow(label: string, a: number, b: number, width = 14): void {
  const pad = (s: string) => s.padStart(width);
  console.log(
    `  ${label.padEnd(16)} ${pad(formatBytes(a))} ${pad(formatBytes(b))}  ${formatDelta(a, b)}`,
  );
}

function printSectionRow(
  label: string,
  a: SectionSizes,
  b: SectionSizes,
  key: keyof SectionSizes,
  width = 14,
): void {
  const av = a[key];
  const bv = b[key];
  if (av == null || bv == null) return;
  printRow(label, av, bv, width);
}

async function main(): Promise<void> {
  const useClang = process.argv.includes("-clang") || defaultUseClang;
  const doClean = process.argv.includes("-clean");
  const fileArg = process.argv
    .slice(2)
    .find((a) => !a.startsWith("-"));

  if (doClean) rmSync(OUT, { recursive: true, force: true });

  await getDeps();

  let testFile = fileArg;
  if (!testFile) {
    const files = corpusFiles();
    if (files.length === 0) {
      console.error("no corpus .djvu found; pass a file path");
      process.exit(1);
    }
    testFile = files[0]!;
  }
  if (!existsSync(testFile)) {
    console.error(`file not found: ${testFile}`);
    process.exit(1);
  }

  const { djvudec, libdjvu } = await buildProbes(useClang);

  const testPath = testFile.replaceAll("\\", "/");

  for (const exe of [djvudec, libdjvu]) {
    const probe = await $`${{ raw: exe }} ${{ raw: testPath }}`.quiet().nothrow();
    if (probe.exitCode !== 0) {
      console.error(`probe failed: ${exe}`);
      console.error(probe.stderr.toString());
      process.exit(1);
    }
  }

  const djvudecBytes = statSync(djvudec).size;
  const libdjvuBytes = statSync(libdjvu).size;
  const djvudecSecs = await readSectionSizes(djvudec);
  const libdjvuSecs = await readSectionSizes(libdjvu);

  const toolchain = useClang ? "clang" : "msvc";
  const opt = useClang ? "-O3" : "-O2 -Ob3 -GL -LTCG";

  console.log("");
  console.log(`Code size comparison (release, ${stripNote(useClang)})`);
  console.log(`  toolchain: ${toolchain} (${opt})`);
  console.log(`  test file: ${testFile}`);
  console.log(`  djvudec:   ${djvudec}`);
  console.log(`  libdjvu:   ${libdjvu}`);
  console.log("");
  console.log(`  ${"".padEnd(16)} ${"djvudec".padStart(14)} ${"libdjvu".padStart(14)}  delta (djvudec - libdjvu)`);
  printRow("executable", djvudecBytes, libdjvuBytes);
  printSectionRow(".text", djvudecSecs, libdjvuSecs, "text");
  if (djvudecSecs.rdata != null || libdjvuSecs.rdata != null) {
    const a =
      (djvudecSecs.text ?? 0) + (djvudecSecs.rdata ?? 0);
    const b =
      (libdjvuSecs.text ?? 0) + (libdjvuSecs.rdata ?? 0);
    printRow(".text+.rdata", a, b);
  }
  printSectionRow(".data", djvudecSecs, libdjvuSecs, "data");
  printSectionRow(".bss", djvudecSecs, libdjvuSecs, "bss");
  printSectionRow("sections total", djvudecSecs, libdjvuSecs, "total");
  console.log("");
  console.log(
    `djvudec is ${djvudecBytes <= libdjvuBytes ? "smaller" : "larger"} by ${formatBytes(Math.abs(djvudecBytes - libdjvuBytes))} ` +
      `(${djvudecBytes <= libdjvuBytes ? "" : "+"}${(((djvudecBytes - libdjvuBytes) / libdjvuBytes) * 100).toFixed(1)}% vs libdjvu executable)`,
  );
}

if (import.meta.main) {
  if (!isWindows && !isMac) {
    console.error(`unsupported platform: ${process.platform}`);
    process.exit(1);
  }
  await main();
}