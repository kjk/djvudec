// build_dump.ts -- build djvudec_dump (library-only inspector, no DjVuLibre).
//
//   bun cmd/build_dump.ts          MSVC on Windows, clang elsewhere
//   bun cmd/build_dump.ts -clang
//   bun cmd/build_dump.ts -clean   delete out/ first
//
// Links src/*.c + test/djvudec_dump.c only. Serves as an API usage example and
// a compilation smoke test for the decoder.
import { $ } from "bun";
import { existsSync, mkdirSync, rmSync, statSync } from "fs";
import { cleanBuildOutput, defaultUseClang, MSVC_CL_C } from "./build";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const OUT_ROOT = `${ROOT}/out`;

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

const outDir = (useClang: boolean) =>
  `${OUT_ROOT}/${useClang ? "clang" : "msvc"}`;

const exeName = (useClang: boolean) =>
  useClang ? "djvudec_dump" : "djvudec_dump.exe";

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

function cUnits(dir: string, ext: string): CompileUnit[] {
  return [
    ...SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${dir}/${objBase(s)}.${ext}`,
    })),
    {
      src: `${ROOT}/test/djvudec_dump.c`,
      obj: `${dir}/djvudec_dump.${ext}`,
    },
  ];
}

async function buildClang(): Promise<string> {
  const dir = outDir(true);
  const exePath = `${dir}/${exeName(true)}`;
  mkdirSync(dir, { recursive: true });

  const units = cUnits(dir, "o");
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src)) continue;
    await $`clang -std=c11 -g -O3 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I${ROOT}/src -c -o ${u.obj} ${u.src}`;
  }

  const objs = units.map((u) => u.obj);
  if (needsRebuild(exePath, ...objs)) {
    await $`clang ${{ raw: objs.join(" ") }} -o ${exePath}`;
  }
  return exePath;
}

async function buildMsvc(): Promise<string> {
  const dir = outDir(false);
  const exePath = `${dir}/${exeName(false)}`;
  mkdirSync(dir, { recursive: true });

  const units = cUnits(dir, "obj");
  const clC = `${MSVC_CL_C} -Isrc -Fo${dir}/ -c`;
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

export async function buildDump(useClang = defaultUseClang): Promise<string> {
  const name = exeName(useClang);
  const exePath = `${outDir(useClang)}/${name}`;
  const units = cUnits(outDir(useClang), useClang ? "o" : "obj");
  const staleObj = units.some((u) => needsRebuild(u.obj, u.src));
  const staleExe = needsRebuild(exePath, ...units.map((u) => u.obj));

  if (!staleObj && !staleExe && existsSync(exePath)) {
    console.log(`${name} up to date`);
    return exePath;
  }

  console.log(`building ${name} (${useClang ? "clang" : "msvc"})...`);
  const exe = useClang ? await buildClang() : await buildMsvc();
  console.log(`built ${name}`);
  return exe;
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  if (args.includes("-clean")) cleanBuildOutput();
  const useClang = args.includes("-clang") || defaultUseClang;
  await buildDump(useClang);
}