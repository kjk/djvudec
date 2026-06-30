// build-dist.ts -- produce an SQLite-style single-file amalgamation in dist/.
//
//   bun cmd/build-dist.ts
//
// Emits two files:
//   dist/djvu.h  -- the public API header (verbatim copy of src/djvu.h)
//   dist/djvu.c  -- the entire decoder as one translation unit: the public
//                   header, then the internal header, then every src/*.c,
//                   concatenated with the local `#include "djvu.h"` /
//                   `#include "djvu_internal.h"` lines stripped out; all C
//                   comments and line-trailing whitespace removed.
//
// A consumer drops both files into their tree and compiles djvu.c like any
// other source. Verifies the result compiles with every available toolchain
// (clang everywhere; MSVC cl.exe too on Windows) before finishing.
// bench.ts calls ensureDist() to regenerate only when src/ is newer than dist/.
import { $ } from "bun";
import { readFileSync, writeFileSync, mkdirSync, existsSync, statSync, rmSync } from "fs";
import { join } from "path";
import { clangCFlags, DJVUDEC_MSVC_CL_C, isWindows } from "./build";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const SRC = join(ROOT, "src");
const DIST = join(ROOT, "dist");

// Same translation-unit order build.ts uses. djvu_internal.h declares every
// cross-module function, so order among the .c files doesn't matter for
// linkage; we keep build.ts's order for a stable, readable amalgamation.
export const DIST_MODULES = [
  "zptable.c",
  "zpcodec.c",
  "bzz.c",
  "bitmap.c",
  "jb2.c",
  "iw44_zigzag.c",
  "iw44.c",
  "scaler.c",
  "compose.c",
  "document.c",
  "bufread.c",
  "render.c",
  "text.c",
  "outline.c",
  "annot.c",
  "debug.c",
];

export const DIST_H = join(DIST, "djvu.h");
export const DIST_C = join(DIST, "djvu.c");

export function distInputPaths(): string[] {
  return [
    join(SRC, "djvu.h"),
    join(SRC, "djvu_internal.h"),
    ...DIST_MODULES.map((name) => join(SRC, name)),
  ];
}

export function distOutdated(): boolean {
  if (!existsSync(DIST_H) || !existsSync(DIST_C)) return true;
  const outMtime = Math.min(statSync(DIST_H).mtimeMs, statSync(DIST_C).mtimeMs);
  for (const input of distInputPaths()) {
    if (!existsSync(input)) return true;
    if (statSync(input).mtimeMs > outMtime) return true;
  }
  return false;
}

// Drop a local-quote include of one of the named headers (system <...>
// includes are left untouched -- they have their own guards).
function stripIncludes(text: string, headers: string[]): string {
  const re = new RegExp(
    `^[ \\t]*#[ \\t]*include[ \\t]+"(?:${headers.join("|")})"[ \\t]*\\r?\\n`,
    "gm",
  );
  return text.replace(re, "");
}

// Remove // and /* */ comments; leaves string/char literal contents intact.
function stripCComments(code: string): string {
  let out = "";
  let i = 0;
  const n = code.length;

  while (i < n) {
    const c = code[i];
    const next = i + 1 < n ? code[i + 1] : "";

    if (c === '"') {
      out += c;
      i++;
      while (i < n) {
        if (code[i] === "\\" && i + 1 < n) {
          out += code[i] + code[i + 1];
          i += 2;
        } else if (code[i] === '"') {
          out += code[i];
          i++;
          break;
        } else {
          out += code[i];
          i++;
        }
      }
      continue;
    }

    if (c === "'") {
      out += c;
      i++;
      while (i < n) {
        if (code[i] === "\\" && i + 1 < n) {
          out += code[i] + code[i + 1];
          i += 2;
        } else if (code[i] === "'") {
          out += code[i];
          i++;
          break;
        } else {
          out += code[i];
          i++;
        }
      }
      continue;
    }

    if (c === "/" && next === "/") {
      i += 2;
      while (i < n && code[i] !== "\n") i++;
      continue;
    }

    if (c === "/" && next === "*") {
      i += 2;
      while (i + 1 < n && !(code[i] === "*" && code[i + 1] === "/")) i++;
      i += 2;
      continue;
    }

    out += c;
    i++;
  }

  return stripTrailingWhitespace(out.replace(/\n{3,}/g, "\n\n"));
}

function stripTrailingWhitespace(code: string): string {
  return code
    .split(/\r?\n/)
    .map((line) => line.replace(/[ \t]+$/, ""))
    .join("\n");
}

/** Echo a compiler command, then run it (stdout progress tracking). */
async function runCmd(cmd: string, cwd?: string): Promise<number> {
  console.log(cwd ? `+ cd ${cwd} && ${cmd}` : `+ ${cmd}`);
  const shell = $`${{ raw: cmd }}`.nothrow();
  if (cwd) await shell.cwd(cwd);
  const r = await shell;
  return r.exitCode ?? 1;
}

async function haveCompiler(name: string): Promise<boolean> {
  const probe = isWindows ? $`where ${name}` : $`which ${name}`;
  const r = await probe.nothrow().quiet();
  return r.exitCode === 0;
}

const VERIFY_OBJS = ["djvu.o", "djvu_verify_clang.o", "djvu_verify_msvc.obj"];

function cleanVerifyObjs(): void {
  for (const name of VERIFY_OBJS) {
    const p = join(DIST, name);
    if (existsSync(p)) rmSync(p);
  }
}

async function verifyDistCompile(toolchain: "clang" | "msvc"): Promise<boolean> {
  const relC = "dist/djvu.c";
  if (toolchain === "clang") {
    const obj = "dist/djvu_verify_clang.o";
    const rc = await runCmd(
      `clang ${clangCFlags("-O1")} -c ${relC} -o ${obj}`,
      ROOT,
    );
    if (existsSync(join(DIST, "djvu_verify_clang.o"))) rmSync(join(DIST, "djvu_verify_clang.o"));
    return rc === 0;
  }
  const obj = "dist/djvu_verify_msvc.obj";
  const rc = await runCmd(`cl ${DJVUDEC_MSVC_CL_C} -c ${relC} -Fo${obj}`, ROOT);
  if (existsSync(join(DIST, "djvu_verify_msvc.obj"))) rmSync(join(DIST, "djvu_verify_msvc.obj"));
  return rc === 0;
}

export async function buildDist(): Promise<void> {
  mkdirSync(DIST, { recursive: true });

  const publicHeader = readFileSync(join(SRC, "djvu.h"), "utf8");
  writeFileSync(DIST_H, publicHeader);

  const parts: string[] = [];
  parts.push(publicHeader.trimEnd() + "\n");

  const internal = readFileSync(join(SRC, "djvu_internal.h"), "utf8");
  parts.push(stripIncludes(internal, ["djvu\\.h"]).trimEnd() + "\n");

  for (const name of DIST_MODULES) {
    const code = readFileSync(join(SRC, name), "utf8");
    parts.push(stripIncludes(code, ["djvu_internal\\.h", "djvu\\.h"]).trimEnd() + "\n");
  }

  const amalgamated = stripCComments(parts.join("\n"));
  writeFileSync(DIST_C, amalgamated);

  const hLines = publicHeader.split("\n").length;
  const cLines = amalgamated.split("\n").length;
  console.log(`wrote dist/djvu.h (${hLines} lines)`);
  console.log(`wrote dist/djvu.c (${cLines} lines, ${DIST_MODULES.length} modules)`);

  cleanVerifyObjs();

  const toolchains: ("clang" | "msvc")[] = [];
  if (await haveCompiler("clang")) toolchains.push("clang");
  else {
    console.error("amalgamation verify: clang not found");
    process.exit(1);
  }
  if (isWindows) {
    if (await haveCompiler("cl")) toolchains.push("msvc");
    else console.log("amalgamation verify: skipping msvc (cl.exe not found)");
  }

  let failed = false;
  for (const tc of toolchains) {
    const label = tc === "clang" ? "clang" : "msvc cl";
    console.log(`verifying dist/djvu.c (${label})...`);
    if (await verifyDistCompile(tc)) console.log(`amalgamation compiles cleanly (${label}) ✓`);
    else {
      console.error(`amalgamation FAILED to compile (${label})`);
      failed = true;
    }
  }
  if (failed) process.exit(1);
}

export async function ensureDist(): Promise<void> {
  if (distOutdated()) {
    console.log("dist/ outdated, regenerating...");
    await buildDist();
  } else {
    console.log("dist/ up to date");
  }
}

if (import.meta.main) {
  await buildDist();
}