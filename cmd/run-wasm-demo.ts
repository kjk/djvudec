// run-wasm-demo.ts — local HTTP server for the WebAssembly DjVu demo.
//
//   bun cmd/run-wasm-demo.ts            # ensure wasm build, serve on :8000
//   bun cmd/run-wasm-demo.ts -port 9000
//   bun cmd/run-wasm-demo.ts -no-build  # skip ensureWasm (serve whatever is there)
//
// Serves:
//   dist/wasm/*          — demo.html, djvu.js, djvu.wasm
//   GET /api/corpus      — JSON list of local corpus .djvu files (from get-deps)
//   GET /corpus/<id>     — raw .djvu bytes for a corpus entry
//
// Open http://localhost:8000/demo.html (redirected from /).
import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { getDeps } from "./get-deps";
import { corpusFiles, fmtBytesHuman } from "./corpus";
import { ensureWasm, WASM_DIR, WASM_JS, WASM_BIN } from "./build-wasm";

const ROOT = path.resolve(import.meta.dir, "..");
const DEMO_HTML = path.join(WASM_DIR, "demo.html");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".djvu": "image/vnd.djvu",
  ".djv": "image/vnd.djvu",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
};

type CorpusEntry = {
  id: string;
  name: string;
  rel: string;
  size: number;
  abs: string;
};

function buildCorpusIndex(): Map<string, CorpusEntry> {
  const map = new Map<string, CorpusEntry>();
  const used = new Set<string>();
  for (const abs of corpusFiles()) {
    const name = path.basename(abs);
    let id = name;
    // Collisions are rare (corpus is basename-deduped) but keep ids unique.
    if (used.has(id)) {
      let n = 2;
      while (used.has(`${name}#${n}`)) n++;
      id = `${name}#${n}`;
    }
    used.add(id);
    let rel = path.relative(ROOT, abs).replaceAll("\\", "/");
    if (rel.startsWith("..") || path.isAbsolute(rel)) rel = abs.replaceAll("\\", "/");
    map.set(id, { id, name, rel, size: statSync(abs).size, abs });
  }
  return map;
}

function staticPath(urlPath: string): string | null {
  // Only serve files under dist/wasm/. Reject ".." and absolute escapes.
  let rel = decodeURIComponent(urlPath.split("?")[0] ?? "");
  if (rel.startsWith("/")) rel = rel.slice(1);
  if (!rel || rel.includes("\0")) return null;
  const parts = rel.split(/[/\\]+/).filter((p) => p && p !== ".");
  if (parts.some((p) => p === "..")) return null;
  const full = path.resolve(WASM_DIR, ...parts);
  if (!full.startsWith(path.resolve(WASM_DIR) + path.sep) && full !== path.resolve(WASM_DIR)) {
    return null;
  }
  return full;
}

function contentType(filePath: string): string {
  return MIME[path.extname(filePath).toLowerCase()] ?? "application/octet-stream";
}

function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}

function text(msg: string, status: number): Response {
  return new Response(msg, {
    status,
    headers: { "content-type": "text/plain; charset=utf-8" },
  });
}

function parseArgs(argv: string[]): { port: number; noBuild: boolean } {
  let port = 8000;
  let noBuild = false;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "-port" || a === "--port") {
      const n = parseInt(argv[++i] ?? "", 10);
      if (!(n > 0 && n < 65536)) {
        console.error("invalid -port");
        process.exit(2);
      }
      port = n;
    } else if (a === "-no-build" || a === "--no-build") {
      noBuild = true;
    } else if (a === "-h" || a === "--help") {
      console.log(`usage: bun cmd/run-wasm-demo.ts [-port N] [-no-build]

Serves dist/wasm/demo.html plus the local corpus .djvu files from deps/
(via get-deps / corpus.ts). Builds the wasm module first unless -no-build.`);
      process.exit(0);
    } else {
      console.error(`unknown arg: ${a}`);
      process.exit(2);
    }
  }
  return { port, noBuild };
}

const { port, noBuild } = parseArgs(process.argv.slice(2));

await getDeps();
if (!noBuild) {
  ensureWasm(false);
} else if (!existsSync(WASM_JS) || !existsSync(WASM_BIN)) {
  console.error("dist/wasm/djvu.js or djvu.wasm missing — run bun cmd/build-wasm.ts");
  process.exit(1);
}
if (!existsSync(DEMO_HTML)) {
  console.error(`missing ${DEMO_HTML}`);
  process.exit(1);
}

const corpus = buildCorpusIndex();
const corpusList = [...corpus.values()]
  .map(({ id, name, rel, size }) => ({ id, name, rel, size }))
  .sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: "base" }));

const server = Bun.serve({
  port,
  async fetch(req) {
    const url = new URL(req.url);
    let pathname = url.pathname;
    if (pathname === "/") {
      return Response.redirect("/demo.html", 302);
    }

    if (pathname === "/api/corpus") {
      return json({ files: corpusList });
    }

    if (pathname.startsWith("/corpus/")) {
      const id = decodeURIComponent(pathname.slice("/corpus/".length));
      const entry = corpus.get(id);
      if (!entry) return text("not found", 404);
      if (!existsSync(entry.abs)) return text("file missing on disk", 404);
      const bytes = readFileSync(entry.abs);
      return new Response(bytes, {
        headers: {
          "content-type": "image/vnd.djvu",
          "content-length": String(bytes.length),
          "cache-control": "public, max-age=3600",
          "content-disposition": `inline; filename="${entry.name.replace(/"/g, "")}"`,
        },
      });
    }

    const filePath = staticPath(pathname);
    if (!filePath || !existsSync(filePath) || !statSync(filePath).isFile()) {
      return text("not found", 404);
    }
    const bytes = readFileSync(filePath);
    return new Response(bytes, {
      headers: {
        "content-type": contentType(filePath),
        "content-length": String(bytes.length),
        // Avoid sticky stale glue/wasm while iterating on the build.
        "cache-control": path.extname(filePath) === ".html" ? "no-store" : "no-cache",
      },
    });
  },
});

const totalBytes = corpusList.reduce((s, f) => s + f.size, 0);
console.log(`wasm demo server  http://localhost:${server.port}/demo.html`);
console.log(`  static:  ${path.relative(ROOT, WASM_DIR).replaceAll("\\", "/")}/`);
console.log(
  `  corpus:  ${corpusList.length} .djvu (${fmtBytesHuman(totalBytes)}) via /api/corpus + /corpus/<id>`,
);
console.log("  Ctrl+C to stop");
