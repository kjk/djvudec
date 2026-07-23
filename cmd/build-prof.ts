// build-prof.ts -- MSVC profile build (no LTCG, with PDB) for WPR/VTune/VS.
//
//   bun cmd/build-prof.ts           # out/msvc_prof/djvudec_prof.exe
//   bun cmd/build-prof.ts -clean    # wipe out/msvc_prof and rebuild
//   bun cmd/build-prof.ts -print    # print workload + WPR one-liners only
//
// Flags: -O2 -Ob1 -Zi (no -GL / no -LTCG) so sampled stacks map to real
// functions. Links test/djvudec_dump.c (same CLI as djvudec_dump).
//
// After build, see printed commands for WPR recording and recommended pages.
import { $ } from "bun";
import { existsSync, mkdirSync, rmSync, statSync } from "fs";
import { isWindows } from "./build";
import { LIB_SRCS } from "./build-lib";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const PROF_DIR = `${ROOT}/out/msvc_prof`;
const EXE = `${PROF_DIR}/djvudec_prof.exe`;
const PDB = `${PROF_DIR}/djvudec_prof.pdb`;
const TEST_SRC = "test/djvudec_dump.c";

// Profile build: optimized but attribute-friendly (no whole-program LTCG).
// -Zi + -DEBUG: full PDB. -Ob1: only explicit inline (clearer stacks than -Ob3).
const PROF_CL =
  `-nologo -O2 -Ob1 -Zi -W4 -WX -std:c11 -MT -D_CRT_SECURE_NO_WARNINGS ` +
  `-Isrc -Fd${PROF_DIR}/ -Fo${PROF_DIR}/ -c`;

const HEADERS = [`${ROOT}/src/djvu.h`, `${ROOT}/src/djvu_internal.h`];

function needsRebuild(output: string, ...inputs: string[]): boolean {
  if (!existsSync(output)) return true;
  const outM = statSync(output).mtimeMs;
  for (const input of inputs) {
    if (!existsSync(input)) return true;
    if (statSync(input).mtimeMs > outM) return true;
  }
  return false;
}

function objPath(src: string): string {
  const base = src.replace(/^src\//, "").replace(/^test\//, "").replace(/\.c$/, "");
  return `${PROF_DIR}/${base}.obj`;
}

function units(): { src: string; obj: string }[] {
  return [
    ...LIB_SRCS.map((s) => ({ src: `${ROOT}/${s}`, obj: objPath(s) })),
    { src: `${ROOT}/${TEST_SRC}`, obj: objPath(TEST_SRC) },
  ];
}

function objStale(u: { src: string; obj: string }): boolean {
  return needsRebuild(u.obj, u.src, ...HEADERS);
}

export async function buildProf(clean = false): Promise<string> {
  if (!isWindows) {
    throw new Error("build-prof.ts is MSVC/Windows-only (use WPR/VTune there)");
  }
  mkdirSync(PROF_DIR, { recursive: true });
  const us = units();
  if (clean) {
    for (const u of us) rmSync(u.obj, { force: true });
    rmSync(EXE, { force: true });
    rmSync(PDB, { force: true });
  }

  const staleObj = us.some(objStale);
  const staleExe = needsRebuild(EXE, ...us.map((u) => u.obj));
  if (!staleObj && !staleExe && existsSync(EXE)) {
    console.log("djvudec_prof.exe up to date");
    return EXE;
  }

  console.log("building djvudec_prof.exe (msvc profile: -O2 -Ob1 -Zi, no LTCG)...");
  for (const u of us) {
    if (!objStale(u)) continue;
    const rel = u.src.startsWith(`${ROOT}/`)
      ? u.src.slice(ROOT.length + 1)
      : u.src;
    await $`cl ${{ raw: PROF_CL }} ${{ raw: rel }}`.cwd(ROOT);
  }
  const objs = us.map((u) => u.obj);
  // -DEBUG writes PDB next to the exe; no -LTCG.
  await $`cl -nologo ${{ raw: objs.join(" ") }} -Fe:${EXE} -link -DEBUG -INCREMENTAL:NO`.cwd(
    ROOT,
  );
  console.log(`built ${EXE}`);
  if (existsSync(PDB)) console.log(`pdb  ${PDB}`);
  return EXE;
}

/** Recommended single-page workloads from a sample-set -layers sweep. */
export const PROF_WORKLOADS = [
  {
    label: "IW44 photo (largest)",
    file: "deps/artifacts/test043C.djvu",
    page: 5,
    why: "~215 ms/page; almost all iw44 — filter_bv / map_image / YCbCr",
  },
  {
    label: "IW44 photo alt",
    file: "deps/artifacts/test043C.djvu",
    page: 1,
    why: "~215 ms; same IW44 path as p5",
  },
  {
    label: "FGbz compound + composite",
    file: "deps/artifacts/test008C.djvu",
    page: 1,
    why: "~170 ms; iw44 dominant, composite stamp also visible",
  },
  {
    label: "FGbz compound (heavy stamp)",
    file: "deps/artifacts/test008C.djvu",
    page: 5,
    why: "high composite share among compound pages (run-aware stamp)",
  },
  {
    label: "Compound FG44 (balanced)",
    file: "deps/DjvuNet/Specs/1998_compression.djvu",
    page: 25,
    why: "~55–60 ms; jb2 + iw44 + composite all non-trivial",
  },
  {
    label: "Bitonal JB2 (cold decode)",
    file: "deps/DjVuLibre/doc/djvu3spec.djvu",
    page: 61,
    why: "~8–11 ms (smaller wall than color) but JB2-dominated; code_bitmap_*/ZP",
  },
  {
    label: "Bitonal JB2 (heaviest on djvu3spec)",
    file: "deps/DjVuLibre/doc/djvu3spec.djvu",
    page: 63,
    why: "top bitonal on this file (~10 ms; jb2+composite stamp)",
  },
] as const;

export function printProfileHelp(exe = EXE): void {
  const q = (s: string) => (/\s/.test(s) ? `"${s}"` : s);
  console.log(`
Profile binary: ${exe}
PDB:            ${PDB}

--- Recommended single-page workloads (from sample-set -layers) ---
`);
  for (const w of PROF_WORKLOADS) {
    console.log(`  ${w.label}`);
    console.log(`    ${w.why}`);
    console.log(
      `    ${q(exe)} -bench-render -layers -reps 5 -page ${w.page} ${w.file}`,
    );
    console.log("");
  }

  console.log(`--- Rank pages yourself ---
  ${q(exe)} -bench-render -layers -reps 2 path\\to\\file.djvu
  # slowest lines: highest pN total ms; layer line shows jb2/iw44/composite

--- WPR (Windows Performance Recorder) CPU sample ---
  # Admin PowerShell recommended
  wpr -start CPU -filemode
  ${q(exe)} -bench-render -reps 8 -page 5 deps/artifacts/test043C.djvu
  wpr -stop %TEMP%\\djvudec.etl

  # Open %TEMP%\\djvudec.etl in Windows Performance Analyzer (WPA):
  #   Computation → CPU Usage (Sampled)
  #   Filter process = djvudec_prof.exe
  #   Stack column → expand djvu_* / filter_bv / code_bitmap_* / compose_*

--- Visual Studio ---
  Debug → Performance Profiler → CPU Usage
  Target: ${exe}
  Args:   -bench-render -reps 5 -page 5 deps/artifacts/test043C.djvu

--- Intel VTune (instructions / CPI per function) ---
  Hotspots or Microarchitecture Exploration
  App:  ${exe}
  Args: -bench-render -reps 5 -page 5 deps/artifacts/test043C.djvu
  # Prefer this for "CPU instructions retired" per function

Notes:
  - Profile build avoids -GL/-LTCG so stacks match source functions.
  - Use -page N so the trace is one workload, not a mixed multipage soup.
  - -reps 5+ so sampling has enough hits; ignore first-run noise or use -warm 0.
`);
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  if (args.includes("-print")) {
    printProfileHelp(existsSync(EXE) ? EXE : "out/msvc_prof/djvudec_prof.exe");
    process.exit(0);
  }
  if (!isWindows) {
    console.error("build-prof.ts requires Windows + MSVC");
    process.exit(1);
  }
  const exe = await buildProf(args.includes("-clean"));
  printProfileHelp(exe);
}
