// build_wasm.ts — build a WebAssembly drop of the djvu decoder into wasm/.
//
//   bun cmd/build_wasm.ts            # incremental build (bootstraps emsdk once)
//   bun cmd/build_wasm.ts -clean     # also wipe/re-activate the local emsdk
//
// Output: wasm/djvu.js  — a self-contained (SINGLE_FILE) Emscripten module that
// embeds the .wasm as base64, so wasm/index.html works even from file://.
// The demo web app (wasm/index.html) is committed alongside and left untouched.
//
// Emscripten isn't assumed to be on PATH: if `emcc` is missing we git-clone the
// emsdk into deps/emsdk and `install/activate latest` there (one-time, cached).

import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, rmSync } from "node:fs";
import path from "node:path";

const ROOT = path.resolve(import.meta.dir, "..");
const SRC = path.join(ROOT, "src");
const WASM = path.join(ROOT, "wasm");
const EMSDK = path.join(ROOT, "deps", "emsdk");
const EMSDK_ENV = path.join(EMSDK, "emsdk_env.sh");

const clean = process.argv.includes("-clean");

// Run a command through bash so we can `source emsdk_env.sh`. Inherits stdio;
// throws (non-zero exit) unless `allowFail`.
function sh(cmd: string, opts: { cwd?: string; allowFail?: boolean; quiet?: boolean } = {}) {
  const r = spawnSync("bash", ["-lc", cmd], {
    cwd: opts.cwd ?? ROOT,
    stdio: opts.quiet ? "pipe" : "inherit",
    encoding: "utf8",
  });
  if (!opts.allowFail && r.status !== 0) {
    if (opts.quiet) process.stderr.write(r.stderr ?? "");
    throw new Error(`command failed (${r.status}): ${cmd}`);
  }
  return r;
}

// Return the shell prefix that puts `emcc` on PATH, or null if we must bootstrap.
function findEmcc(): string | null {
  // Already on PATH (user has their own emsdk activated)?
  if (spawnSync("bash", ["-lc", "command -v emcc"], { encoding: "utf8" }).status === 0) {
    return "";
  }
  // Locally bootstrapped emsdk?
  if (existsSync(EMSDK_ENV)) {
    const r = spawnSync("bash", ["-lc", `source ${q(EMSDK_ENV)} >/dev/null 2>&1 && command -v emcc`], {
      encoding: "utf8",
    });
    if (r.status === 0) return `source ${q(EMSDK_ENV)} >/dev/null 2>&1 && `;
  }
  return null;
}

const q = (s: string) => `'${s.replace(/'/g, `'\\''`)}'`;

function bootstrapEmsdk() {
  console.log("• emcc not found — bootstrapping emsdk into deps/emsdk (one-time)…");
  mkdirSync(path.dirname(EMSDK), { recursive: true });
  if (!existsSync(path.join(EMSDK, ".git"))) {
    sh(`git clone --depth 1 https://github.com/emscripten-core/emsdk.git ${q(EMSDK)}`);
  }
  sh(`./emsdk install latest`, { cwd: EMSDK });
  sh(`./emsdk activate latest`, { cwd: EMSDK });
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

function build(prefix: string) {
  mkdirSync(WASM, { recursive: true });
  const cfiles = readdirSync(SRC)
    .filter((f) => f.endsWith(".c"))
    .map((f) => q(path.join(SRC, f)))
    .join(" ");
  const out = q(path.join(WASM, "djvu.js"));
  const flags = [
    "-O2",
    `-I ${q(SRC)}`,
    "-sMODULARIZE=1",
    "-sEXPORT_NAME=createDjvuModule",
    "-sSINGLE_FILE=1",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sENVIRONMENT=web",
    "-sEXPORT_ES6=0",
    `-sEXPORTED_FUNCTIONS=${EXPORTS.join(",")}`,
    `-sEXPORTED_RUNTIME_METHODS=${RUNTIME.join(",")}`,
  ].join(" ");
  console.log("• compiling " + readdirSync(SRC).filter((f) => f.endsWith(".c")).length + " C files → wasm/djvu.js");
  sh(`${prefix}emcc ${cfiles} ${flags} -o ${out}`);
  const kb = (Bun.file(path.join(WASM, "djvu.js")).size / 1024).toFixed(0);
  console.log(`✓ wrote wasm/djvu.js (${kb} KB, wasm embedded)`);
  console.log("  open the demo:  cd wasm && python3 -m http.server 8000   → http://localhost:8000/");
  console.log("  (or just open wasm/index.html directly — SINGLE_FILE works from file://)");
}

// -clean wipes the cached emsdk before we look for emcc.
if (clean) rmSync(EMSDK, { recursive: true, force: true });
const prefix = ensureEmcc();
build(prefix);
