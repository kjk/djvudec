// build-dist.ts -- produce an SQLite-style single-file amalgamation in dist/.
//
//   bun cmd/build-dist.ts            write dist/, verify it compiles, build WASM
//   bun cmd/build-dist.ts -no-wasm   write dist/ + verify only (no emsdk, no
//                                    dist/wasm rebuild) -- what CI's host jobs run
//
// Emits:
//   dist/djvu.h  -- the public API header (verbatim copy of src/djvu.h)
//   dist/djvu.c  -- the entire decoder as one translation unit: the public
//                   header, then the internal header, then every src/*.c,
//                   concatenated with the local `#include "djvu.h"` /
//                   `#include "djvu_internal.h"` lines stripped out; all C
//                   comments and line-trailing whitespace removed.
//   dist/wasm/djvu.js + djvu.wasm -- Emscripten modularize split module for the
//                   committed demo at dist/wasm/demo.html; compiled from dist/djvu.c.
//                   Serve with `bun cmd/run-wasm-demo.ts` (file:// cannot load .wasm).
//
// A consumer drops both files into their tree and compiles djvu.c like any
// other source. Verifies the result compiles with every available toolchain
// (clang everywhere; MSVC cl.exe too on Windows) before finishing; the
// throwaway verify objects go to out/dist_verify/, never into dist/.
//
// Only this script (run directly) writes dist/. Other scripts that want the
// amalgamation -- bench.ts via buildBench() -- call ensureAmalgamation(dir)
// to generate the same djvu.h + djvu.c into a directory under out/, so they
// never dirty the tracked dist/ files and never trigger a WASM build.
import { $ } from "bun";
import { readFileSync, writeFileSync, mkdirSync, rmSync } from "fs";
import { relative } from "path";
import {
  clangCFlags,
  DJVUDEC_MSVC_CL_C,
  INTERNAL_H,
  isWindows,
  LIB_SRCS,
  msvcFd,
  needsRebuild,
  PUBLIC_H,
  ROOT,
} from "./util";
import { buildWasm } from "./build-wasm";

const DIST = `${ROOT}/dist`;
const VERIFY_DIR = `${ROOT}/out/dist_verify`;

/** The two files of an amalgamation generated into `dir`. */
export function amalgPaths(dir: string): { h: string; c: string } {
  return { h: `${dir}/djvu.h`, c: `${dir}/djvu.c` };
}

// Inputs, in concatenation order: public header, internal header, then the
// modules in LIB_SRCS order (see util.ts -- the order is the layout of djvu.c).
const AMALG_INPUTS = [PUBLIC_H, INTERNAL_H, ...LIB_SRCS.map((s) => `${ROOT}/${s}`)];

/** True when dir/djvu.h or dir/djvu.c is missing or older than any src/ input. */
export function amalgOutdated(dir: string): boolean {
  const { h, c } = amalgPaths(dir);
  return needsRebuild(h, ...AMALG_INPUTS) || needsRebuild(c, ...AMALG_INPUTS);
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

/** LF only, strip trailing whitespace, at most one blank line in a row. */
function normalizeSourceText(text: string): string {
  const lines = text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").split("\n");
  const out: string[] = [];
  let blank = false;
  for (const raw of lines) {
    const line = raw.replace(/[ \t]+$/, "");
    if (line.length === 0) {
      if (blank) continue;
      blank = true;
      out.push("");
    } else {
      blank = false;
      out.push(line);
    }
  }
  // Drop leading blank lines; keep a single trailing newline.
  while (out.length > 0 && out[0] === "") out.shift();
  while (out.length > 0 && out[out.length - 1] === "") out.pop();
  return out.length === 0 ? "\n" : out.join("\n") + "\n";
}

// Remove // and /* */ comments; leaves string/char literal contents intact.
function stripCComments(code: string): string {
  // Normalize EOLs first so comment/blank handling never sees CRLF.
  code = code.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
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

  return normalizeSourceText(out);
}

/** Path relative to the repo root, forward slashes (for logs and cl args). */
const rel = (p: string) => relative(ROOT, p).replaceAll("\\", "/");

/** Generate the amalgamation (djvu.h + djvu.c) into `dir`. Writes nothing else. */
export function writeAmalgamation(dir: string): { h: string; c: string } {
  mkdirSync(dir, { recursive: true });
  const { h, c } = amalgPaths(dir);

  const publicHeader = normalizeSourceText(readFileSync(PUBLIC_H, "utf8"));
  writeFileSync(h, publicHeader);

  const parts: string[] = [publicHeader];
  parts.push(stripIncludes(readFileSync(INTERNAL_H, "utf8"), ["djvu\.h"]));
  for (const src of LIB_SRCS) {
    const code = readFileSync(`${ROOT}/${src}`, "utf8");
    parts.push(stripIncludes(code, ["djvu_internal\.h", "djvu\.h"]));
  }

  // Final pass also collapses blank runs left at module boundaries.
  const amalgamated = stripCComments(parts.join("\n"));
  writeFileSync(c, amalgamated);

  console.log(`wrote ${rel(h)} (${publicHeader.split("\n").length} lines)`);
  console.log(
    `wrote ${rel(c)} (${amalgamated.split("\n").length} lines, ${LIB_SRCS.length} modules)`,
  );
  return { h, c };
}

/** Regenerate dir/djvu.{h,c} only when src/ is newer (no verify, no WASM). */
export function ensureAmalgamation(dir: string): { h: string; c: string } {
  if (amalgOutdated(dir)) return writeAmalgamation(dir);
  return amalgPaths(dir);
}

/** Echo a compiler command, run it, return its exit code (never throws). */
async function tryCmd(cmd: string, cwd: string): Promise<number> {
  console.log(`+ cd ${cwd} && ${cmd}`);
  const r = await $`${{ raw: cmd }}`.cwd(cwd).nothrow();
  return r.exitCode ?? 1;
}

async function haveCompiler(name: string): Promise<boolean> {
  const probe = isWindows ? $`where ${name}` : $`which ${name}`;
  const r = await probe.nothrow().quiet();
  return r.exitCode === 0;
}

/** Compile dir/djvu.c with every available toolchain; throws if one fails. */
export async function verifyAmalgamation(dir: string): Promise<void> {
  const src = rel(amalgPaths(dir).c);
  const objDir = rel(VERIFY_DIR);
  rmSync(VERIFY_DIR, { recursive: true, force: true });
  mkdirSync(VERIFY_DIR, { recursive: true });

  if (!(await haveCompiler("clang"))) {
    throw new Error("amalgamation verify: clang not found");
  }
  const toolchains: { label: string; cmd: string }[] = [
    {
      label: "clang",
      cmd: `clang ${clangCFlags("-O1")} -c ${src} -o ${objDir}/djvu_verify_clang.o`,
    },
  ];
  if (isWindows) {
    if (await haveCompiler("cl")) {
      toolchains.push({
        label: "msvc cl",
        // -Fd keeps the intermediate compiler PDB (-Zi) out of the repo root.
        cmd: `cl ${DJVUDEC_MSVC_CL_C} -c ${src} -Fo${objDir}/djvu_verify_msvc.obj ${msvcFd(objDir)}`,
      });
    } else console.log("amalgamation verify: skipping msvc (cl.exe not found)");
  }

  const failed: string[] = [];
  for (const tc of toolchains) {
    console.log(`verifying ${src} (${tc.label})...`);
    if ((await tryCmd(tc.cmd, ROOT)) === 0) {
      console.log(`amalgamation compiles cleanly (${tc.label}) ✓`);
    } else {
      console.error(`amalgamation FAILED to compile (${tc.label})`);
      failed.push(tc.label);
    }
  }
  rmSync(VERIFY_DIR, { recursive: true, force: true });
  if (failed.length) {
    throw new Error(`amalgamation does not compile with: ${failed.join(", ")}`);
  }
}

/** Write dist/djvu.{h,c}, verify they compile, and (unless wasm=false) rebuild
    dist/wasm from them. This is the only code path that writes dist/. */
export async function buildDist(opts: { wasm?: boolean } = {}): Promise<void> {
  writeAmalgamation(DIST);
  await verifyAmalgamation(DIST);
  if (opts.wasm ?? true) buildWasm({ useDist: true });
  else console.log("skipping WASM build (-no-wasm)");
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const known = ["-no-wasm"];
  const bad = args.filter((a) => !known.includes(a));
  if (bad.length) {
    console.error(`unknown argument(s): ${bad.join(" ")}`);
    console.error("usage: bun cmd/build-dist.ts [-no-wasm]");
    process.exit(2);
  }
  try {
    await buildDist({ wasm: !args.includes("-no-wasm") });
  } catch (e) {
    console.error(e instanceof Error ? e.message : e);
    process.exit(1);
  }
}
