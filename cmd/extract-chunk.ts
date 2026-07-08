#!/usr/bin/env bun
// extract-chunk.ts -- dump a raw chunk payload from a FORM inside a .djvu file.
//
//   bun cmd/extract-chunk.ts file.djvu <form_offset> <chunk_id> <out.raw>
//
// form_offset is the component offset as printed by `djvudec_dump -comps`
// (points at the "FORM" tag). Writes the first chunk with the given 4-char id.
import { readFileSync, writeFileSync } from "fs";

const [file, offStr, chunkId, out] = process.argv.slice(2);
if (!file || !offStr || !chunkId || !out) {
  console.error("usage: bun cmd/extract-chunk.ts file.djvu <form_offset> <chunk_id> <out.raw>");
  process.exit(2);
}
const data = readFileSync(file);
const off = parseInt(offStr, 10);
const tag = (p: number) => data.toString("latin1", p, p + 4);
const u32 = (p: number) => data.readUInt32BE(p);

if (tag(off) !== "FORM") {
  console.error(`no FORM at offset ${off} (found "${tag(off)}")`);
  process.exit(1);
}
const formEnd = Math.min(off + 8 + u32(off + 4), data.length);
let p = off + 12; // skip FORM + size + form type
while (p + 8 <= formEnd) {
  const id = tag(p);
  const csz = u32(p + 4);
  const cdata = p + 8;
  if (id === chunkId) {
    writeFileSync(out, data.subarray(cdata, cdata + csz));
    console.log(`wrote ${csz} bytes of ${chunkId} to ${out}`);
    process.exit(0);
  }
  p = cdata + csz + (csz & 1);
}
console.error(`chunk ${chunkId} not found in FORM at ${off}`);
process.exit(1);
