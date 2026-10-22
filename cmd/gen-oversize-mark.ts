// Generate a crafted single-page .djvu whose JB2 (Sjbz) stream codes a
// REC_NewMark shape of 65535x65535. djvu_bm_init must size the bitmap with
// 64-bit arithmetic and reject the overflow; the pre-fix code truncated the
// product to a ~128 KB allocation and then wrote decoded pixels out of bounds
// (reported by Seokhyun Ann, Oct 2026).
//
// libdjvu can't encode this (GBitmap::init rejects 65535x65535 outright), so
// this reimplements the DjVuLibre ZP-coder encoder (ZPCODER variant, see
// deps/DjVuLibre/libdjvu/ZPCodec.cpp) plus JB2 CodeNum (JB2Image.cpp), both
// matching the decoder in src/zpcodec.c + src/jb2.c.
//
//   bun cmd/gen-oversize-mark.ts [out.djvu]

import { writeFileSync, readFileSync } from "fs";

// --- ZP default table (parsed from src/zptable.c) ---
const tbl = readFileSync(new URL("../src/zptable.c", import.meta.url), "utf8");
const P: number[] = [],
  M: number[] = [],
  UP: number[] = [],
  DN: number[] = [];
for (const m of tbl.matchAll(
  /\{\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(\d+),\s*(\d+)\s*\}/g,
)) {
  P.push(parseInt(m[1], 16));
  M.push(parseInt(m[2], 16));
  UP.push(parseInt(m[3]));
  DN.push(parseInt(m[4]));
}
if (P.length !== 256)
  throw new Error(`zptable: parsed ${P.length} entries, expected 256`);

// --- ZP encoder (DjVuLibre ZPCodec, encoding path, ZPCODER build) ---
class ZPEncoder {
  out: number[] = [];
  a = 0;
  byte = 0;
  scount = 0;
  delay = 25;
  subend = 0;
  buffer = 0xffffff;
  nrun = 0;

  private outbit(bit: number) {
    if (this.delay > 0) {
      if (this.delay < 0xff) this.delay -= 1;
    } else {
      this.byte = ((this.byte << 1) | bit) & 0xff;
      if (++this.scount === 8) {
        this.out.push(this.byte);
        this.scount = 0;
        this.byte = 0;
      }
    }
  }

  private zemit(b: number) {
    this.buffer = ((this.buffer << 1) + b) >>> 0;
    const carry = this.buffer >>> 24;
    this.buffer = this.buffer & 0xffffff;
    if (carry === 1) {
      this.outbit(1);
      while (this.nrun-- > 0) this.outbit(0);
      this.nrun = 0;
    } else if (carry === 0xff) {
      this.outbit(0);
      while (this.nrun-- > 0) this.outbit(1);
      this.nrun = 0;
    } else if (carry === 0) {
      this.nrun += 1;
    } else {
      throw new Error("zemit: bad carry " + carry);
    }
  }

  // ctx is a {s} box so adaptation writes back into the caller's context slot
  encode(bit: number, ctx: { s: number }) {
    const z = this.a + P[ctx.s];
    if (bit !== (ctx.s & 1)) this.encodeLps(ctx, z);
    else if (z >= 0x8000) this.encodeMps(ctx, z);
    else this.a = z;
  }

  private encodeMps(ctx: { s: number }, z: number) {
    const d = 0x6000 + ((z + this.a) >> 2);
    if (z > d) z = d;
    if (this.a >= M[ctx.s]) ctx.s = UP[ctx.s];
    this.a = z;
    if (this.a >= 0x8000) {
      this.zemit(1 - (this.subend >> 15));
      this.subend = (this.subend << 1) & 0xffff;
      this.a = (this.a << 1) & 0xffff;
    }
  }

  private encodeLps(ctx: { s: number }, z: number) {
    const d = 0x6000 + ((z + this.a) >> 2);
    if (z > d) z = d;
    ctx.s = DN[ctx.s];
    z = 0x10000 - z;
    this.subend += z;
    this.a += z;
    while (this.a >= 0x8000) {
      this.zemit(1 - (this.subend >> 15));
      this.subend = (this.subend << 1) & 0xffff;
      this.a = (this.a << 1) & 0xffff;
    }
  }

  flush(): Uint8Array {
    if (this.subend > 0x8000) this.subend = 0x10000;
    else if (this.subend > 0) this.subend = 0x8000;
    while (this.buffer !== 0xffffff || this.subend) {
      this.zemit(1 - (this.subend >> 15));
      this.subend = (this.subend << 1) & 0xffff;
    }
    this.outbit(1);
    while (this.nrun-- > 0) this.outbit(0);
    this.nrun = 0;
    while (this.scount > 0) this.outbit(1);
    this.delay = 0xff;
    return new Uint8Array(this.out);
  }
}

// --- JB2 coder: CodeNum + single-bit flags (JB2Image.cpp encode path) ---
class Jb2 {
  zp = new ZPEncoder();
  bitcells = [0]; // shared cell pool; index 0 reserved
  leftcell = [0];
  rightcell = [0];
  roots: Record<string, number> = {}; // NumContext distribution roots
  flags: Record<string, number> = {}; // single-byte BitContexts

  private codeBit(cellIdx: number, bit: number): number {
    const ctx = { s: this.bitcells[cellIdx] };
    this.zp.encode(bit, ctx);
    this.bitcells[cellIdx] = ctx.s;
    return bit;
  }

  flag(name: string, bit: number) {
    const ctx = { s: this.flags[name] ?? 0 };
    this.zp.encode(bit, ctx);
    this.flags[name] = ctx.s;
  }

  // Mirror of JB2Codec::CodeNum (encoding). `pctx` is a getter/setter pair
  // over the current distribution slot (root int, or a cell's left/right).
  codeNum(low: number, high: number, root: string, value: number) {
    let get = () => this.roots[root] ?? 0;
    let set = (v: number) => (this.roots[root] = v);
    let v = value;
    let negative = false;
    let cutoff = 0;

    for (let phase = 1, range = 0xffffffff; range !== 1;) {
      if (get() === 0) {
        this.bitcells.push(0);
        this.leftcell.push(0);
        this.rightcell.push(0);
        set(this.bitcells.length - 1);
      }
      const cell = get();

      const bitCoded = low < cutoff && high >= cutoff;
      const decision = bitCoded
        ? !!this.codeBit(cell, v >= cutoff ? 1 : 0)
        : v >= cutoff;

      if (decision) {
        get = () => this.rightcell[cell];
        set = (x: number) => (this.rightcell[cell] = x);
      } else {
        get = () => this.leftcell[cell];
        set = (x: number) => (this.leftcell[cell] = x);
      }

      switch (phase) {
        case 1:
          negative = !decision;
          if (negative) {
            v = -v - 1;
            const t = -low - 1;
            low = -high - 1;
            high = t;
          }
          phase = 2;
          cutoff = 1;
          break;
        case 2:
          if (!decision) {
            phase = 3;
            range = (cutoff + 1) >> 1;
            if (range === 1) cutoff = 0;
            else cutoff -= range >> 1;
          } else {
            cutoff += cutoff + 1;
          }
          break;
        case 3:
          range = Math.trunc(range / 2);
          if (range !== 1) {
            if (!decision) cutoff -= range >> 1;
            else cutoff += range >> 1;
          } else if (!decision) {
            cutoff -= 1;
          }
          break;
      }
    }
  }

  flush(): Uint8Array {
    return this.zp.flush();
  }
}

// record types (src/jb2.c)
const REC_StartOfData = 0,
  REC_NewMark = 1,
  REC_EndOfData = 11;
const BIGPOSITIVE = 262142;

function buildSjbz(): Uint8Array {
  const j = new Jb2();

  // Record 1: StartOfData (image). image size 16x16, no eventual refinement.
  j.codeNum(REC_StartOfData, REC_EndOfData, "rectype", REC_StartOfData);
  j.codeNum(0, BIGPOSITIVE, "imagesize", 16); // columns
  j.codeNum(0, BIGPOSITIVE, "imagesize", 16); // rows
  j.flag("refine", 0);

  // Record 2: NewMark with an abstract 65535x65535 mark -> overflow in init.
  j.codeNum(REC_StartOfData, REC_EndOfData, "rectype", REC_NewMark);
  j.codeNum(0, BIGPOSITIVE, "abs_size_x", 65535);
  j.codeNum(0, BIGPOSITIVE, "abs_size_y", 65535);
  // Decoder aborts here (fixed) or writes OOB in code_bitmap_directly (old).

  return j.flush();
}

// --- DjVu container ---
const asc = (s: string) => s.split("").map((c) => c.charCodeAt(0));
const u16be = (n: number) => [(n >> 8) & 0xff, n & 0xff];
const u16le = (n: number) => [n & 0xff, (n >> 8) & 0xff];
const u32be = (n: number) => [
  (n >>> 24) & 0xff,
  (n >>> 16) & 0xff,
  (n >>> 8) & 0xff,
  n & 0xff,
];

function chunk(id: string, body: number[]): number[] {
  const b = [...asc(id), ...u32be(body.length), ...body];
  if (body.length & 1) b.push(0); // IFF even-pad
  return b;
}

function buildDjvu(sjbz: Uint8Array): Uint8Array {
  // INFO: width/height u16be, minor/major u8, dpi u16le, gamma u8, flags u8
  const info = [...u16be(16), ...u16be(16), 24, 0, ...u16le(300), 22, 0];
  const djvuBody = [
    ...asc("DJVU"),
    ...chunk("INFO", info),
    ...chunk("Sjbz", Array.from(sjbz)),
  ];
  const form = [...asc("FORM"), ...u32be(djvuBody.length), ...djvuBody];
  return new Uint8Array([...asc("AT&T"), ...form]);
}

const out = process.argv[2] ?? "oversize-mark.djvu";
const file = buildDjvu(buildSjbz());
writeFileSync(out, file);
console.log(`wrote ${out} (${file.length} bytes)`);
