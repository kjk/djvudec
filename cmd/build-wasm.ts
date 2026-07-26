// build-wasm.ts — build a WebAssembly drop of the djvu decoder into dist/wasm/.
//
//   bun cmd/build-wasm.ts            # incremental build (bootstraps emsdk once)
//   bun cmd/build-wasm.ts -clean     # also wipe/re-activate the local emsdk
//
// Output (split module — needs an HTTP server; use `bun cmd/run-wasm-demo.ts`):
//   dist/wasm/djvu.js
//   dist/wasm/djvu.wasm
// The demo page (dist/wasm/demo.html) is committed alongside and left untouched.
//
// Emscripten isn't assumed to be on PATH: if `emcc` is missing we git-clone the
// emsdk into deps/emsdk and `install/activate latest` there (one-time, cached).
// build-dist.ts calls buildWasm({ useDist: true }) to compile dist/djvu.c.

import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, rmSync, statSync } from "node:fs";
import path from "node:path";

const ROOT = path.resolve(import.meta.dir, "..");
const SRC = path.join(ROOT, "src");
const DIST_C = path.join(ROOT, "dist", "djvu.c");
export const WASM_DIR = path.join(ROOT, "dist", "wasm");
export const WASM_JS = path.join(WASM_DIR, "djvu.js");
export const WASM_BIN = path.join(WASM_DIR, "djvu.wasm");
const EMSDK = path.join(ROOT, "deps", "emsdk");
const isWin = process.platform === "win32";
const EMSDK_ENV = path.join(EMSDK, isWin ? "emsdk_env.bat" : "emsdk_env.sh");

// Quote a path for the platform shell (skip quotes on Windows when unnecessary —
// cmd.exe /s /c treats embedded quotes literally and breaks git clone).
const q = (s: string) => {
  if (isWin) return /[\s"]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  return `'${s.replace(/'/g, `'\\''`)}'`;
};

// Run a shell command. On Windows uses cmd.exe + emsdk_env.bat; elsewhere bash.
function sh(cmd: string, opts: { cwd?: string; allowFail?: boolean; quiet?: boolean } = {}) {
  const cwd = opts.cwd ?? ROOT;
  const stdio = opts.quiet ? ("pipe" as const) : ("inherit" as const);
  // A git-bash parent leaks MSYSTEM into the env, which makes emsdk scripts
  // think they run in an MSYS shell and print `export` lines instead of
  // setting cmd.exe's PATH — emcc then never lands on PATH.
  const env = { ...process.env };
  delete env.MSYSTEM;
  const r = isWin
    ? spawnSync("cmd.exe", ["/d", "/s", "/c", cmd], { cwd, stdio, encoding: "utf8", env })
    : spawnSync("bash", ["-lc", cmd], { cwd, stdio, encoding: "utf8", env });
  if (!opts.allowFail && r.status !== 0) {
    if (opts.quiet) process.stderr.write(r.stderr ?? "");
    throw new Error(`command failed (${r.status}): ${cmd}`);
  }
  return r;
}

function emccOnPath(): boolean {
  if (isWin) {
    return spawnSync("where", ["emcc"], { shell: true, encoding: "utf8" }).status === 0;
  }
  return spawnSync("bash", ["-lc", "command -v emcc"], { encoding: "utf8" }).status === 0;
}

// Return the shell prefix that puts `emcc` on PATH, or null if we must bootstrap.
function findEmcc(): string | null {
  if (emccOnPath()) return "";
  if (!existsSync(EMSDK_ENV)) return null;
  if (isWin) {
    const env = { ...process.env };
    delete env.MSYSTEM; // see sh()
    const r = spawnSync("cmd.exe", ["/d", "/s", "/c", `call ${q(EMSDK_ENV)} >nul 2>&1 && where emcc`], {
      encoding: "utf8",
      env,
    });
    if (r.status === 0) return `call ${q(EMSDK_ENV)} >nul 2>&1 && `;
  } else {
    const r = spawnSync("bash", ["-lc", `source ${q(EMSDK_ENV)} >/dev/null 2>&1 && command -v emcc`], {
      encoding: "utf8",
    });
    if (r.status === 0) return `source ${q(EMSDK_ENV)} >/dev/null 2>&1 && `;
  }
  return null;
}

function bootstrapEmsdk() {
  console.log("• emcc not found — bootstrapping emsdk into deps/emsdk (one-time)…");
  mkdirSync(path.dirname(EMSDK), { recursive: true });
  if (!existsSync(path.join(EMSDK, ".git"))) {
    sh(`git clone --depth 1 https://github.com/emscripten-core/emsdk.git ${q(EMSDK)}`);
  }
  // Explicit path: with NoDefaultCurrentDirectoryInExePath set, cmd.exe won't
  // resolve a bare "emsdk.bat" against the cwd.
  const emsdk = isWin ? q(path.join(EMSDK, "emsdk.bat")) : "./emsdk";
  sh(`${emsdk} install latest`, { cwd: EMSDK });
  sh(`${emsdk} activate latest`, { cwd: EMSDK });
}

function ensureEmcc(): string {
  let prefix = findEmcc();
  if (prefix === null) {
    bootstrapEmsdk();
    prefix = findEmcc();
    if (prefix === null) throw new Error("emsdk bootstrap did not produce a working emcc");
  }
  const v = sh(`${prefix}emcc --version`, { quiet: true });
  console.log("• using " + (v.stdout ?? "").split("\n")[0]);
  return prefix;
}

// The C entry points the web app calls, plus malloc/free for the input buffer.
const EXPORTS = [
  "_djvu_ctx_new",
  "_djvu_ctx_free",
  "_djvu_doc_open",
  "_djvu_doc_close",
  "_djvu_doc_page_count",
  "_djvu_doc_page_info",
  "_djvu_page_get_type",
  "_djvu_page_render",
  "_djvu_image_destroy",
  "_malloc",
  "_free",
];

const RUNTIME = ["HEAP8", "HEAPU8", "HEAP32", "HEAPU32"];

function wasmInputMtime(useDist: boolean): number {
  if (useDist) {
    if (!existsSync(DIST_C)) throw new Error("dist/djvu.c missing — run build-dist first");
    return statSync(DIST_C).mtimeMs;
  }
  let newest = 0;
  for (const f of readdirSync(SRC)) {
    if (!f.endsWith(".c")) continue;
    newest = Math.max(newest, statSync(path.join(SRC, f)).mtimeMs);
  }
  for (const h of ["djvu.h", "djvu_internal.h"]) {
    const p = path.join(SRC, h);
    if (existsSync(p)) newest = Math.max(newest, statSync(p).mtimeMs);
  }
  return newest;
}

export function wasmOutdated(useDist = false): boolean {
  if (!existsSync(WASM_JS) || !existsSync(WASM_BIN)) return true;
  const outMtime = Math.min(statSync(WASM_JS).mtimeMs, statSync(WASM_BIN).mtimeMs);
  return outMtime < wasmInputMtime(useDist);
}

function compile(prefix: string, useDist: boolean) {
  mkdirSync(WASM_DIR, { recursive: true });
  const out = q(WASM_JS);
  // Split output: djvu.js + djvu.wasm (no SINGLE_FILE). Needs an HTTP server —
  // file:// cannot fetch the companion .wasm (use `bun cmd/run-wasm-demo.ts`).
  const flags = [
    "-O2",
    "-sMODULARIZE=1",
    "-sEXPORT_NAME=createDjvuModule",
    "-sALLOW_MEMORY_GROWTH=1",
    // web for the browser demo; node so verify-wasm.ts / Bun can load the .wasm
    // via locateFile without needing fetch() of a URL.
    "-sENVIRONMENT=web,node",
    "-sEXPORT_ES6=0",
    `-sEXPORTED_FUNCTIONS=${EXPORTS.join(",")}`,
    `-sEXPORTED_RUNTIME_METHODS=${RUNTIME.join(",")}`,
  ].join(" ");

  let inputs: string;
  if (useDist) {
    inputs = q(DIST_C);
    console.log("• compiling dist/djvu.c → dist/wasm/djvu.js + djvu.wasm");
  } else {
    const cfiles = readdirSync(SRC)
      .filter((f) => f.endsWith(".c"))
      .map((f) => q(path.join(SRC, f)))
      .join(" ");
    inputs = `${cfiles} -I ${q(SRC)}`;
    const n = readdirSync(SRC).filter((f) => f.endsWith(".c")).length;
    console.log(`• compiling ${n} C files → dist/wasm/djvu.js + djvu.wasm`);
  }

  sh(`${prefix}emcc ${inputs} ${flags} -o ${out}`);
  if (!existsSync(WASM_BIN)) {
    throw new Error(`emcc did not write ${WASM_BIN}`);
  }
  const jsKb = (Bun.file(WASM_JS).size / 1024).toFixed(0);
  const wasmKb = (Bun.file(WASM_BIN).size / 1024).toFixed(0);
  console.log(`✓ wrote dist/wasm/djvu.js (${jsKb} KB) + dist/wasm/djvu.wasm (${wasmKb} KB)`);
  console.log("  open the demo:  bun cmd/run-wasm-demo.ts   → http://localhost:8000/demo.html");
}

export function buildWasm(opts: { useDist?: boolean; cleanEmsdk?: boolean } = {}): void {
  const useDist = opts.useDist ?? false;
  if (opts.cleanEmsdk) rmSync(EMSDK, { recursive: true, force: true });
  const prefix = ensureEmcc();
  compile(prefix, useDist);
}

export function ensureWasm(useDist = false): void {
  if (!wasmOutdated(useDist)) {
    console.log("dist/wasm/ up to date");
    return;
  }
  buildWasm({ useDist });
}

if (import.meta.main) {
  const clean = process.argv.includes("-clean");
  buildWasm({ cleanEmsdk: clean });
}
