// verify-wasm.ts — smoke-test wasm/djvu.js by decoding a file through the same
// exports the web app uses, then cross-check dims against djvu_test.
//   bun cmd/verify-wasm.ts [file.djvu]
import path from "node:path";
import { readFileSync } from "node:fs";

const ROOT = path.resolve(import.meta.dir, "..");
const file = process.argv[2] ?? path.join(ROOT, "testfiles/full/lizard2002.djvu");

// The glue is built for ENVIRONMENT=web; provide the browser globals it probes.
globalThis.self = globalThis as any;
const createDjvuModule = (await import(path.join(ROOT, "wasm/djvu.js"))).default;
const M: any = await createDjvuModule();

const ctx = M._djvu_ctx_new(0, 0, 0, 0, 0, 0);
const bytes = new Uint8Array(readFileSync(file));
const buf = M._malloc(bytes.length);
M.HEAPU8.set(bytes, buf);
const doc = M._djvu_doc_open(ctx, buf, bytes.length);
if (!doc) throw new Error("doc_open failed");
const n = M._djvu_doc_page_count(doc);
console.log(`${path.basename(file)}: ${n} page(s)`);

const info = M._malloc(20);
M._djvu_doc_page_info(doc, 0, info);
const iw = M.HEAP32[info >> 2], ih = M.HEAP32[(info >> 2) + 1];
console.log(`page 0 info: ${iw}x${ih}`);
M._free(info);

const img = M._djvu_page_render(doc, 0, 1);
if (!img) throw new Error("render failed");
const w = M.HEAP32[img >> 2], h = M.HEAP32[(img >> 2) + 1];
const fmt = M.HEAP32[(img >> 2) + 2], stride = M.HEAP32[(img >> 2) + 3];
const data = M.HEAPU32[(img >> 2) + 4];
// sanity: not a uniform image
let min = 255, max = 0;
for (let i = 0; i < w * h; i += 997) { const v = M.HEAPU8[data + i]; if (v < min) min = v; if (v > max) max = v; }
console.log(`render: ${w}x${h} fmt=${fmt} stride=${stride} sampled[min=${min} max=${max}]`);
if (w !== iw || h !== ih) throw new Error("render dims != info dims");
if (min === max) throw new Error("uniform image — decode likely broken");
console.log("✓ wasm decode OK");

M._djvu_image_destroy(ctx, img);
M._djvu_doc_close(doc);
M._free(buf);
M._djvu_ctx_free(ctx);
