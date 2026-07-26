// prof.ts -- sampling profile of our page render on Windows via ../winperf.
//
//   bun cmd/prof.ts <file.djvu> [-page N] [-runs N] [-hz N] [-sub N]
//
// Builds the profile binary (MSVC -O2 -Ob1 -Zi, no LTCG → out/msvc_prof/
// djvudec_prof.exe) then records it under winperf and prints the agent report:
// top self-time functions, hot source lines, and the heaviest call path.
//
// Windows only. winperf drives xperf (Windows Performance Toolkit / ADK) and
// needs Administrator rights (UAC prompt). The harness uses winperf section
// marks (-profile N) so samples outside render are dropped.
//
// Build winperf once:
//   cd ../winperf && bun cmd/build.ts -release
// Or clone if missing:
//   git clone https://github.com/kjk/winperf ..\winperf
import { existsSync, mkdirSync } from "fs";
import { dirname, resolve } from "path";
import { isWindows } from "./build";
import { buildProf, PROF_WORKLOADS } from "./build-prof";

const ROOT = dirname(import.meta.dir);

// DJVUDEC_WINPERF overrides; otherwise try the usual sibling checkout paths.
const WINPERF = (() => {
  const env = process.env.DJVUDEC_WINPERF;
  if (env && existsSync(env)) return env;
  for (const p of [
    "../winperf/out/rel64/winperf.exe",
    "../winperf/out/dbg64/winperf.exe",
    "../winperf/out/winperf.exe",
  ]) {
    const abs = resolve(ROOT, p);
    if (existsSync(abs)) return abs;
  }
  return resolve(ROOT, "../winperf/out/rel64/winperf.exe");
})();

const argv = process.argv.slice(2);
const flagVal = (name: string, dflt: string) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
};
const RUNS = flagVal("-runs", "10");
const HZ = flagVal("-hz", "4000");
const PAGE = flagVal("-page", flagVal("-p", "1"));
const SUB = flagVal("-sub", "1");
// Only these take a value; treating every flag as if it did would swallow
// the filename after a boolean one.
const VALUE_FLAGS = ["-runs", "-hz", "-page", "-p", "-sub"];
const files = argv.filter(
  (a, i) => !a.startsWith("-") && !VALUE_FLAGS.includes(argv[i - 1] ?? ""),
);

function usage(): void {
  console.error(`usage: bun cmd/prof.ts <file.djvu> [-page N] [-runs N] [-hz N] [-sub N]
  -page N   1-based page to render (default 1)
  -runs N   render loops inside the profiled process (default 10)
  -hz N     sampling rate (default 4000)
  -sub N    render subsample (default 1)

Windows only: winperf records with xperf and needs Administrator rights
(and the Windows Performance Toolkit from the ADK).

Recommended workloads (from sample-set -layers):`);
  for (const w of PROF_WORKLOADS.slice(0, 4)) {
    console.error(`  ${w.label}: -page ${w.page} ${w.file}`);
  }
  console.error(`
Build winperf:  cd ../winperf && bun cmd/build.ts -release
Clone if missing: git clone https://github.com/kjk/winperf ..\\winperf`);
}

if (!isWindows || files.length !== 1) {
  usage();
  process.exit(2);
}
if (!existsSync(WINPERF)) {
  console.error(`prof: ${WINPERF} not found -- build it with
  cd ../winperf && bun cmd/build.ts -release
Or clone first:
  git clone https://github.com/kjk/winperf ..\\winperf`);
  process.exit(2);
}

const file = resolve(files[0]);
if (!existsSync(file)) {
  console.error(`prof: no such file: ${files[0]}`);
  process.exit(1);
}

const EXE = await buildProf(false);
const outDir = resolve(ROOT, "out/prof");
mkdirSync(outDir, { recursive: true });
// Keep the .etl and Firefox profile JSON inside out/, which is gitignored.
const OUT = resolve(outDir, "winperf.etl");

console.log(`prof: ${WINPERF}`);
console.log(`  exe:  ${EXE}`);
console.log(`  work: -profile ${RUNS} -page ${PAGE} -sub ${SUB} ${file}`);
console.log(`  out:  ${OUT}`);

const proc = Bun.spawnSync({
  // Absolute paths: like samply, relative attachment is unreliable and can
  // produce a trace full of unrelated system processes instead of erroring.
  cmd: [
    WINPERF,
    "record",
    "-i",
    HZ,
    "-o",
    OUT,
    "-print-agent",
    "--",
    resolve(EXE),
    "-profile",
    RUNS,
    "-page",
    PAGE,
    "-sub",
    SUB,
    file,
  ],
  stdout: "inherit",
  stderr: "inherit",
  cwd: ROOT,
});
process.exitCode = proc.exitCode ?? 1;
