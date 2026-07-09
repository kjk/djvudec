// djvu-parse.ts -- cheap structural parser for the DjVu file format.
//
// Walks the IFF container and chunk headers WITHOUT decoding/decompressing
// any image data (no ZP, no BZZ, no JB2/IW44). Gathers everything that is
// cheap to read: page count, per-page resolution/dpi/rotation, chunk
// inventory with sizes, IW44 header info (dimensions, color vs gray), FGbz
// palette size, presence of text/annotation/outline chunks, shared and
// inline JB2 dictionaries, thumbnails, and legacy codecs (Smmr, BGjp/FGjp).
// The derived `features` list gives each file a fingerprint usable to pick a
// minimal subset of files covering all DjVu features.
//
// Layouts mirror src/document.c (IFF walk, DIRM plain header, INFO),
// src/iw44.c (IW44 chunk header), and src/compose.c (FGbz header).
//
//   import { parseDjvu } from "./djvu-parse";
//   const info = parseDjvu(new Uint8Array(readFileSync(f)));

export interface ChunkInfo {
  id: string;
  size: number;
  offset: number; // file offset of the chunk id tag
}

// From the first chunk of a BG44/FG44/PM44 sequence (serial 0).
export interface Iw44Info {
  chunks: number; // progressive refinement chunk count
  totalBytes: number;
  width: number;
  height: number;
  version: { major: number; minor: number };
  color: boolean; // false = grayscale-only stream (major bit 0x80 set)
  crcbDelay: number; // chroma delay in slices (-1 = no chroma)
  slices: number; // total slices across all chunks
}

export interface FgbzInfo {
  version: number;
  colors: number; // palette entry count
  hasShapeTable: boolean; // per-blit color indices present
  bytes: number;
}

export type PageKind = "bitonal" | "photo" | "compound" | "blank" | "unknown";

export interface PageInfo {
  formOffset: number;
  formSize: number;
  width: number;
  height: number;
  version: number;
  dpi: number;
  gamma: number;
  rotation: 0 | 90 | 180 | 270;
  kind: PageKind;
  chunks: ChunkInfo[];
  sjbzBytes: number; // JB2 mask (0 = none)
  smmrBytes: number; // legacy MMR/G4 mask
  djbzBytes: number; // inline JB2 dictionary
  incl: string[]; // INCL component ids referenced
  bg44: Iw44Info | null; // IW44 background
  fg44: Iw44Info | null; // IW44 foreground
  bgjpBytes: number; // legacy JPEG background
  fgjpBytes: number; // legacy JPEG foreground
  fgbz: FgbzInfo | null; // foreground palette
  txtzBytes: number; // BZZ-compressed hidden text
  txtaBytes: number; // plain hidden text
  antzBytes: number; // BZZ-compressed annotations
  antaBytes: number; // plain annotations
}

export interface ComponentInfo {
  offset: number;
  size: number;
  formType: string; // DJVU | DJVI | THUM | ...
}

export interface DjvuInfo {
  formType: "DJVU" | "DJVM" | "PM44" | "BM44"; // PM44/BM44 = standalone IW44 image
  fileBytes: number;
  bundled: boolean; // DJVM only
  dirmVersion: number; // DJVM only (0 for single-page)
  componentCount: number;
  components: ComponentInfo[];
  pages: PageInfo[];
  sharedDictBytes: number; // total Djbz bytes in DJVI components
  sharedDicts: number; // DJVI components containing a Djbz
  thumbnails: number; // THUM components
  navmBytes: number; // document outline (0 = none)
  sharedAnnotBytes: number; // ANTa/ANTz in DJVI components
  features: string[]; // deduped fingerprint for subset picking
}

const td = new TextDecoder("latin1");

function tag(d: Uint8Array, p: number): string {
  if (p + 4 > d.length) return "";
  return td.decode(d.subarray(p, p + 4));
}

function u16be(d: Uint8Array, p: number): number {
  return (d[p]! << 8) | d[p + 1]!;
}

function u16le(d: Uint8Array, p: number): number {
  return d[p]! | (d[p + 1]! << 8);
}

function u32be(d: Uint8Array, p: number): number {
  return ((d[p]! << 24) | (d[p + 1]! << 16) | (d[p + 2]! << 8) | d[p + 3]!) >>> 0;
}

// Walk the sub-chunks of a FORM at formOff. Mirrors document.c's walkers,
// including the overflow-safe size clamp (a crafted chunk size must not send
// the walk backwards).
function* chunks(d: Uint8Array, formOff: number): Generator<ChunkInfo> {
  if (tag(d, formOff) !== "FORM") return;
  let formEnd = formOff + 8 + u32be(d, formOff + 4);
  if (formEnd > d.length) formEnd = d.length;
  let pos = formOff + 12;
  while (pos + 8 <= formEnd) {
    const id = tag(d, pos);
    let size = u32be(d, pos + 4);
    const data = pos + 8;
    if (size > formEnd - data) size = formEnd - data; // overflow-safe clamp
    yield { id, size, offset: pos };
    pos = data + size + (size & 1);
  }
}

function parseIw44Header(d: Uint8Array, off: number, size: number): Omit<Iw44Info, "chunks" | "totalBytes"> | null {
  // serial(1) slices(1) major(1) minor(1) w(2be) h(2be) [crcbdelay(1)]
  const p = off + 8;
  if (size < 8 || d[p] !== 0) return null; // serial must be 0 on the first chunk
  const slices = d[p + 1]!;
  const major = d[p + 2]!;
  const minor = d[p + 3]!;
  if ((major & 0x7f) !== 1) return null;
  const width = u16be(d, p + 4);
  const height = u16be(d, p + 6);
  let crcbDelay = 10;
  if (minor >= 2 && size >= 9) crcbDelay = d[p + 8]! & 0x7f;
  const color = !(major & 0x80);
  return {
    width,
    height,
    version: { major: major & 0x7f, minor },
    color,
    crcbDelay: color ? crcbDelay : -1,
    slices,
  };
}

function parseFgbz(d: Uint8Array, off: number, size: number): FgbzInfo | null {
  const p = off + 8;
  if (size < 3) return null;
  const version = d[p]!;
  const colors = u16be(d, p + 1);
  return {
    version: version & 0x7f,
    colors,
    hasShapeTable: size > 3 + colors * 3,
    bytes: size,
  };
}

function parsePage(d: Uint8Array, formOff: number): PageInfo {
  const pg: PageInfo = {
    formOffset: formOff,
    formSize: u32be(d, formOff + 4),
    width: 0,
    height: 0,
    version: 0,
    dpi: 300,
    gamma: 2.2,
    rotation: 0,
    kind: "unknown",
    chunks: [],
    sjbzBytes: 0,
    smmrBytes: 0,
    djbzBytes: 0,
    incl: [],
    bg44: null,
    fg44: null,
    bgjpBytes: 0,
    fgjpBytes: 0,
    fgbz: null,
    txtzBytes: 0,
    txtaBytes: 0,
    antzBytes: 0,
    antaBytes: 0,
  };

  for (const c of chunks(d, formOff)) {
    pg.chunks.push(c);
    const p = c.offset + 8;
    switch (c.id) {
      case "INFO":
        if (c.size >= 5) {
          pg.width = u16be(d, p);
          pg.height = u16be(d, p + 2);
          pg.version = d[p + 4]!;
        }
        if (c.size >= 8) pg.dpi = u16le(d, p + 6) || 300;
        if (c.size >= 9) pg.gamma = d[p + 8]! / 10;
        if (c.size >= 10) {
          const flag = d[p + 9]! & 0x7;
          pg.rotation = flag === 6 ? 90 : flag === 2 ? 180 : flag === 5 ? 270 : 0;
        }
        break;
      case "Sjbz":
        pg.sjbzBytes += c.size;
        break;
      case "Smmr":
        pg.smmrBytes += c.size;
        break;
      case "Djbz":
        pg.djbzBytes += c.size;
        break;
      case "INCL": {
        // chunk body is the referenced component id (NUL/whitespace-trimmed)
        let s = td.decode(d.subarray(p, p + c.size));
        s = s.replace(/[\0\r\n]+$/, "").trim();
        if (s) pg.incl.push(s);
        break;
      }
      case "BG44":
      case "FG44": {
        const slot = c.id === "BG44" ? "bg44" : "fg44";
        const cur = pg[slot];
        if (!cur) {
          const hdr = parseIw44Header(d, c.offset, c.size);
          if (hdr) pg[slot] = { ...hdr, chunks: 1, totalBytes: c.size };
          else pg[slot] = {
            chunks: 1, totalBytes: c.size, width: 0, height: 0,
            version: { major: 0, minor: 0 }, color: false, crcbDelay: -1, slices: 0,
          };
        } else {
          cur.chunks++;
          cur.totalBytes += c.size;
          if (c.size >= 2) cur.slices += d[c.offset + 8 + 1]!;
        }
        break;
      }
      case "BGjp":
        pg.bgjpBytes += c.size;
        break;
      case "FGjp":
        pg.fgjpBytes += c.size;
        break;
      case "FGbz":
        pg.fgbz = parseFgbz(d, c.offset, c.size);
        break;
      case "TXTz":
        pg.txtzBytes += c.size;
        break;
      case "TXTa":
        pg.txtaBytes += c.size;
        break;
      case "ANTz":
        pg.antzBytes += c.size;
        break;
      case "ANTa":
        pg.antaBytes += c.size;
        break;
    }
  }

  const hasMask = pg.sjbzBytes > 0 || pg.smmrBytes > 0;
  const hasBg = pg.bg44 !== null || pg.bgjpBytes > 0;
  const hasFg = pg.fg44 !== null || pg.fgjpBytes > 0 || pg.fgbz !== null;
  if (hasMask && (hasBg || hasFg)) pg.kind = "compound";
  else if (hasMask) pg.kind = "bitonal";
  else if (hasBg) pg.kind = "photo";
  else if (pg.width > 0) pg.kind = "blank";
  return pg;
}

export function parseDjvu(data: Uint8Array): DjvuInfo {
  const d = data;
  let pos = 0;
  if (tag(d, 0) === "AT&T") pos = 4;
  if (tag(d, pos) !== "FORM") throw new Error("not a DjVu file (no FORM)");
  const formType = tag(d, pos + 8);

  const info: DjvuInfo = {
    formType: formType === "DJVM" ? "DJVM" : "DJVU",
    fileBytes: d.length,
    bundled: false,
    dirmVersion: 0,
    componentCount: 0,
    components: [],
    pages: [],
    sharedDictBytes: 0,
    sharedDicts: 0,
    thumbnails: 0,
    navmBytes: 0,
    sharedAnnotBytes: 0,
    features: [],
  };

  if (formType === "DJVU") {
    info.componentCount = 1;
    info.components.push({ offset: pos, size: u32be(d, pos + 4), formType: "DJVU" });
    info.pages.push(parsePage(d, pos));
  } else if (formType === "PM44" || formType === "BM44") {
    // standalone IW44 image: FORM:PM44 (color) / FORM:BM44 (gray) holding a
    // sequence of PM44/BM44 refinement chunks; no INFO chunk.
    info.formType = formType;
    info.componentCount = 1;
    info.components.push({ offset: pos, size: u32be(d, pos + 4), formType });
    const pg = parsePage(d, pos); // collects the chunk list; INFO stays empty
    for (const c of pg.chunks) {
      if (c.id !== "PM44" && c.id !== "BM44") continue;
      if (!pg.bg44) {
        const hdr = parseIw44Header(d, c.offset, c.size);
        if (hdr) pg.bg44 = { ...hdr, chunks: 1, totalBytes: c.size };
      } else {
        pg.bg44.chunks++;
        pg.bg44.totalBytes += c.size;
        if (c.size >= 2) pg.bg44.slices += d[c.offset + 8 + 1]!;
      }
    }
    if (pg.bg44) {
      pg.width = pg.bg44.width;
      pg.height = pg.bg44.height;
    }
    pg.kind = "photo";
    info.pages.push(pg);
  } else if (formType === "DJVM") {
    // DIRM plain header: flag byte (bit7 bundled, low7 version), u16 count,
    // then (bundled) count u32be component offsets. The per-component
    // sizes/flags/ids that follow are BZZ-compressed -- not needed here; each
    // component is classified by the FORM type at its offset instead.
    for (const c of chunks(d, pos)) {
      if (c.id === "DIRM" && c.size >= 3) {
        const p = c.offset + 8;
        const flag = d[p]!;
        info.bundled = !!(flag & 0x80);
        info.dirmVersion = flag & 0x7f;
        const count = u16be(d, p + 1);
        info.componentCount = count;
        if (info.bundled) {
          for (let i = 0; i < count && p + 3 + i * 4 + 4 <= p + c.size; i++) {
            const off = u32be(d, p + 3 + i * 4);
            if (off + 12 > d.length || tag(d, off) !== "FORM") continue;
            info.components.push({
              offset: off,
              size: u32be(d, off + 4),
              formType: tag(d, off + 8),
            });
          }
        }
      } else if (c.id === "NAVM") {
        info.navmBytes += c.size;
      }
    }
    for (const comp of info.components) {
      if (comp.formType === "DJVU") {
        info.pages.push(parsePage(d, comp.offset));
      } else if (comp.formType === "DJVI") {
        for (const c of chunks(d, comp.offset)) {
          if (c.id === "Djbz") {
            info.sharedDicts++;
            info.sharedDictBytes += c.size;
          } else if (c.id === "ANTa" || c.id === "ANTz") {
            info.sharedAnnotBytes += c.size;
          }
        }
      } else if (comp.formType === "THUM") {
        info.thumbnails++;
      }
    }
  } else {
    throw new Error(`unsupported FORM type: ${formType}`);
  }

  info.features = deriveFeatures(info);
  return info;
}

// Deduped feature fingerprint: which decoder paths this file exercises.
function deriveFeatures(info: DjvuInfo): string[] {
  const f = new Set<string>();
  if (info.formType === "DJVM") f.add(info.bundled ? "djvm-bundled" : "djvm-indirect");
  else if (info.formType === "PM44" || info.formType === "BM44") f.add("standalone-iw44");
  else f.add("single-page");
  if (info.pages.length > 1) f.add("multi-page");
  if (info.sharedDicts > 0) f.add("shared-dict");
  if (info.thumbnails > 0) f.add("thumbnails");
  if (info.navmBytes > 0) f.add("outline");
  if (info.sharedAnnotBytes > 0) f.add("shared-anno");
  for (const pg of info.pages) {
    f.add(`page-${pg.kind}`);
    if (pg.sjbzBytes) f.add("jb2");
    if (pg.smmrBytes) f.add("smmr");
    if (pg.djbzBytes) f.add("inline-dict");
    if (pg.incl.length) f.add("incl");
    if (pg.bg44) {
      f.add(pg.bg44.color ? "bg44-color" : "bg44-gray");
      if (pg.bg44.chunks > 1) f.add("iw44-progressive");
    }
    if (pg.fg44) f.add("fg44");
    if (pg.fgbz) f.add("fgbz-palette");
    if (pg.fgbz?.hasShapeTable) f.add("fgbz-shapetable");
    if (pg.bgjpBytes || pg.fgjpBytes) f.add("jpeg-layers");
    if (pg.txtzBytes) f.add("text-txtz");
    if (pg.txtaBytes) f.add("text-txta");
    if (pg.antzBytes || pg.antaBytes) f.add("anno");
    if (pg.rotation !== 0) f.add("rotated");
    if (pg.gamma !== 2.2) f.add("nonstandard-gamma");
  }
  return [...f].sort();
}
