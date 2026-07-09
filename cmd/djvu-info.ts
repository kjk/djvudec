// djvu-info.ts -- print cheap structural info about .djvu files.
//
//   bun cmd/djvu-info.ts <file.djvu ... | -rand N | -all> [-features]
//
// Uses cmd/djvu-parse.ts (no image decoding): container/page layout, chunk
// inventory, per-page resolution and kind (bitonal/photo/compound), IW44
// header info, palette sizes, text/annotation/outline presence, and the
// derived feature fingerprint used to pick a feature-covering test subset.
// -features prints one compact `file<TAB>features` line per file instead.
import { readFileSync } from "fs";
import { dirname } from "path";
import { getDeps } from "./get-deps";
import { corpusSummary, fileLabel, fmtBytesHuman, selectFiles } from "./corpus";
import { parseDjvu, type DjvuInfo, type PageInfo } from "./djvu-parse";

const ROOT = dirname(import.meta.dir);

function pageLine(pg: PageInfo, no: number): string {
  const parts: string[] = [];
  parts.push(`${pg.width}x${pg.height}`);
  parts.push(`dpi=${pg.dpi}`);
  if (pg.rotation) parts.push(`rot=${pg.rotation}`);
  if (pg.gamma !== 2.2) parts.push(`gamma=${pg.gamma.toFixed(1)}`);
  parts.push(pg.kind);
  if (pg.sjbzBytes) parts.push(`jb2=${fmtBytesHuman(pg.sjbzBytes)}`);
  if (pg.smmrBytes) parts.push(`smmr=${fmtBytesHuman(pg.smmrBytes)}`);
  if (pg.djbzBytes) parts.push(`inline-dict=${fmtBytesHuman(pg.djbzBytes)}`);
  if (pg.incl.length) parts.push(`incl=[${pg.incl.join(",")}]`);
  if (pg.bg44)
    parts.push(
      `bg44=${pg.bg44.width}x${pg.bg44.height} ${pg.bg44.color ? "color" : "gray"} ` +
        `${pg.bg44.chunks} chunk(s) ${fmtBytesHuman(pg.bg44.totalBytes)}`,
    );
  if (pg.fg44)
    parts.push(
      `fg44=${pg.fg44.width}x${pg.fg44.height} ${pg.fg44.color ? "color" : "gray"} ` +
        `${fmtBytesHuman(pg.fg44.totalBytes)}`,
    );
  if (pg.fgbz)
    parts.push(
      `fgbz=${pg.fgbz.colors} color(s)${pg.fgbz.hasShapeTable ? "+shapes" : ""}`,
    );
  if (pg.bgjpBytes || pg.fgjpBytes) parts.push("jpeg-layers");
  if (pg.txtzBytes) parts.push(`txtz=${fmtBytesHuman(pg.txtzBytes)}`);
  if (pg.txtaBytes) parts.push(`txta=${fmtBytesHuman(pg.txtaBytes)}`);
  if (pg.antzBytes) parts.push(`antz=${fmtBytesHuman(pg.antzBytes)}`);
  if (pg.antaBytes) parts.push(`anta=${fmtBytesHuman(pg.antaBytes)}`);
  return `  page ${no}: ${parts.join(" ")}`;
}

function printInfo(info: DjvuInfo): void {
  const doc: string[] = [];
  doc.push(
    info.formType === "DJVM"
      ? (info.bundled ? "DJVM bundled" : "DJVM indirect")
      : info.formType === "PM44" || info.formType === "BM44"
        ? `standalone IW44 (${info.formType})`
        : "DJVU single-page",
  );
  doc.push(`${info.pages.length} page(s)`);
  if (info.componentCount !== info.pages.length)
    doc.push(`${info.componentCount} component(s)`);
  if (info.sharedDicts)
    doc.push(`${info.sharedDicts} shared dict(s) (${fmtBytesHuman(info.sharedDictBytes)})`);
  if (info.thumbnails) doc.push(`${info.thumbnails} thumbnail(s)`);
  if (info.navmBytes) doc.push(`outline (${fmtBytesHuman(info.navmBytes)})`);
  if (info.sharedAnnotBytes) doc.push(`shared anno (${fmtBytesHuman(info.sharedAnnotBytes)})`);
  console.log(`  ${doc.join(", ")}`);
  info.pages.forEach((pg, i) => console.log(pageLine(pg, i + 1)));
  console.log(`  features: ${info.features.join(" ")}`);
}

const featuresOnly = process.argv.includes("-features");

await getDeps();
const files = selectFiles(
  `usage: bun cmd/djvu-info.ts <selection> [options]
selection (required; default prints this help):
  file.djvu ...   print info about the given files
  -rand N         print info about N randomly selected corpus files
  -all            print info about every corpus file
options:
  -features       one compact "file<TAB>features" line per file

${corpusSummary()}`,
);

let failed = 0;
for (const f of files) {
  try {
    const info = parseDjvu(new Uint8Array(readFileSync(f)));
    if (featuresOnly) {
      console.log(`${fileLabel(f, ROOT)}\t${info.pages.length} page(s)\t${info.features.join(" ")}`);
    } else {
      console.log(fileLabel(f, ROOT));
      printInfo(info);
    }
  } catch (e) {
    failed++;
    const msg = e instanceof Error ? e.message : String(e);
    console.log(`${fileLabel(f, ROOT)}\n  PARSE FAILED: ${msg}`);
  }
}
if (files.length > 1)
  console.log(`\n${files.length} file(s), ${failed} parse failure(s)`);
process.exit(failed ? 1 : 0);
