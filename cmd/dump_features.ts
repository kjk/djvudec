#!/usr/bin/env bun
// dump_features.ts -- scan .djvu files and dump per-file/page features + render times.
//
//   bun cmd/dump_features.ts [dir]              dump to features.jsonl (default: testfiles/full)
//   bun cmd/dump_features.ts --pick [dir]       dump, pick minimal subset, copy to testfiles/subset
//   bun cmd/dump_features.ts --pick-only file   pick subset from existing features.jsonl
//
// Uses djvu_test -dump-features (one doc open per file). Output: features.jsonl plus
// features_summary.txt listing unique feature tags across the corpus.
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  statSync,
  unlinkSync,
  writeFileSync,
} from "fs";
import { cpus } from "os";
import { join, dirname, basename, relative } from "path";
import { getDeps } from "./get-deps";
import { build, defaultUseClang } from "./build";

const ROOT = dirname(import.meta.dir);
const DEFAULT_DIR = join(ROOT, "testfiles", "full");
const OUT_JSONL = join(ROOT, "features.jsonl");
const OUT_SUMMARY = join(ROOT, "features_summary.txt");
const SUBSET_DIR = join(ROOT, "testfiles", "subset");

type PageFeat = {
  page: number;
  type: string;
  kind: string;
  rot: number;
  text: boolean;
  annot: boolean;
  links: boolean;
  incl: boolean;
  inlineDjbz: boolean;
  inclDjbz: boolean;
  fgbz: boolean;
  fg44: boolean;
  bgjp: boolean;
  fgjp: boolean;
  renderMs: number;
};

type FileFeat = {
  path: string;
  rel: string;
  bytes: number;
  pages: PageFeat[];
  pagesCount: number;
  container: string;
  ncompDjvi: number;
  incl: boolean;
  inclDjbz: boolean;
  inlineDjbz: boolean;
  outline: boolean;
  text: boolean;
  annot: boolean;
  links: boolean;
  typeBitonal: boolean;
  typePhoto: boolean;
  typeCompound: boolean;
  kindMask: boolean;
  kindBg: boolean;
  kindOther: boolean;
  fgbz: boolean;
  fg44: boolean;
  bgjp: boolean;
  fgjp: boolean;
  rotNonzero: boolean;
  totalRenderMs: number;
  tags: string[];
  error?: string;
};

function walkDjvu(dir: string): string[] {
  if (!existsSync(dir)) return [];
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...walkDjvu(p));
    else if (name.toLowerCase().endsWith(".djvu")) out.push(p);
  }
  return out.sort();
}

function parseDump(out: string): Omit<FileFeat, "path" | "rel" | "bytes" | "tags"> {
  const pages: PageFeat[] = [];
  let summary: Record<string, string> = {};

  for (const line of out.split(/\r?\n/)) {
    if (!line) continue;
    const p = line.split("\t");
    if (p[0] === "page" && p.length >= 22) {
      pages.push({
        page: parseInt(p[1], 10),
        type: p[3],
        kind: p[5],
        rot: parseInt(p[7], 10) || 0,
        text: p[9] === "1",
        annot: p[11] === "1",
        links: p[13] === "1",
        incl: p[15] === "1",
        inlineDjbz: p[17] === "1",
        inclDjbz: p[19] === "1",
        fgbz: p[21] === "1",
        fg44: p[23] === "1",
        bgjp: p[25] === "1",
        fgjp: p[27] === "1",
        renderMs: parseFloat(p[29]) || 0,
      });
    } else if (p[0] === "summary") {
      for (let i = 1; i + 1 < p.length; i += 2) summary[p[i]] = p[i + 1];
    }
  }

  const num = (k: string) => parseInt(summary[k] ?? "0", 10) > 0;
  const fl = (k: string) => parseFloat(summary[k] ?? "0") || 0;

  return {
    pages,
    pagesCount: parseInt(summary.pages ?? "0", 10),
    container: summary.container ?? "unknown",
    ncompDjvi: parseInt(summary.ncomp_djvi ?? "0", 10),
    incl: num("incl"),
    inclDjbz: num("incl_djbz"),
    inlineDjbz: num("inline_djbz"),
    outline: num("outline"),
    text: num("text"),
    annot: num("annot"),
    links: num("links"),
    typeBitonal: num("type_bitonal"),
    typePhoto: num("type_photo"),
    typeCompound: num("type_compound"),
    kindMask: num("kind_mask"),
    kindBg: num("kind_bg"),
    kindOther: num("kind_other"),
    fgbz: num("fgbz"),
    fg44: num("fg44"),
    bgjp: num("bgjp"),
    fgjp: num("fgjp"),
    rotNonzero: num("rot_nonzero"),
    totalRenderMs: fl("total_render_ms"),
  };
}

function featureTags(f: FileFeat): string[] {
  const tags: string[] = [`container:${f.container}`];
  if (f.ncompDjvi > 0) tags.push("djvi");
  if (f.incl) tags.push("incl");
  if (f.inclDjbz) tags.push("incl_djbz");
  if (f.inlineDjbz) tags.push("inline_djbz");
  if (f.outline) tags.push("outline");
  if (f.text) tags.push("text");
  if (f.annot) tags.push("annot");
  if (f.links) tags.push("links");
  if (f.typeBitonal) tags.push("type:bitonal");
  if (f.typePhoto) tags.push("type:photo");
  if (f.typeCompound) tags.push("type:compound");
  if (f.kindMask) tags.push("kind:mask");
  if (f.kindBg) tags.push("kind:bg");
  if (f.kindOther) tags.push("kind:other");
  if (f.fgbz) tags.push("fgbz");
  if (f.fg44) tags.push("fg44");
  if (f.bgjp) tags.push("bgjp");
  if (f.fgjp) tags.push("fgjp");
  if (f.rotNonzero) tags.push("rotation");
  return tags;
}

// Codec/regression fixtures we always want in the subset when present.
const MUST_INCLUDE = new Set([
  "1998_zcoder.djvu",
  "1998_compression.djvu",
  "1998_lossy_masked.djvu",
  "1998_distribution.djvu",
  "Mcguffey's_Primer.djvu",
  "djvu3spec.djvu",
  "djvu2spec.djvu",
  "bug-3125-rotated-pages.djvu",
  "bug-2200-probability-7.djvu",
]);

function pickSubset(all: FileFeat[]): FileFeat[] {
  const ok = all.filter((f) => !f.error && f.pagesCount > 0);
  const universe = new Set<string>();
  for (const f of ok) for (const t of f.tags) universe.add(t);

  const picked: FileFeat[] = [];
  const covered = new Set<string>();

  for (const f of ok) {
    if (MUST_INCLUDE.has(basename(f.path))) {
      picked.push(f);
      for (const t of f.tags) covered.add(t);
    }
  }

  const remaining = ok.filter((f) => !picked.includes(f));
  while (covered.size < universe.size) {
    let best: FileFeat | null = null;
    let bestScore = -1;
    for (const f of remaining) {
      if (picked.includes(f)) continue;
      const newTags = f.tags.filter((t) => !covered.has(t));
      if (!newTags.length) continue;
      // Prefer more new tags, then faster total render.
      const score = newTags.length * 1e6 - f.totalRenderMs;
      if (score > bestScore) {
        bestScore = score;
        best = f;
      }
    }
    if (!best) break;
    picked.push(best);
    for (const t of best.tags) covered.add(t);
  }

  return picked.sort((a, b) => a.rel.localeCompare(b.rel));
}

function writeSummary(all: FileFeat[], picked: FileFeat[] | null) {
  const tagCounts = new Map<string, number>();
  for (const f of all) {
    if (f.error) continue;
    for (const t of f.tags) tagCounts.set(t, (tagCounts.get(t) ?? 0) + 1);
  }
  const lines: string[] = [
    `files scanned: ${all.length}`,
    `files ok: ${all.filter((f) => !f.error).length}`,
    `unique tags: ${tagCounts.size}`,
    "",
    "tag\tfiles",
    ...[...tagCounts.entries()].sort((a, b) => a[0].localeCompare(b[0])).map(([t, n]) => `${t}\t${n}`),
  ];
  if (picked) {
    const ms = picked.reduce((s, f) => s + f.totalRenderMs, 0);
    lines.push(
      "",
      `subset: ${picked.length} files, ${picked.reduce((s, f) => s + f.pagesCount, 0)} pages, ${ms.toFixed(1)} ms render`,
      "",
      "picked\tpages\trender_ms\ttags",
      ...picked.map(
        (f) =>
          `${f.rel}\t${f.pagesCount}\t${f.totalRenderMs.toFixed(1)}\t${f.tags.join(",")}`,
      ),
    );
  }
  writeFileSync(OUT_SUMMARY, lines.join("\n") + "\n");
}

async function dumpDir(dir: string, workers: number): Promise<FileFeat[]> {
  await getDeps();
  const useClang = process.argv.includes("-clang") || defaultUseClang;
  const TEST = await build(useClang);
  const files = walkDjvu(dir);
  console.log(`dumping features for ${files.length} files with ${workers} workers...`);

  const results: FileFeat[] = new Array(files.length);
  let next = 0;
  let done = 0;

  async function worker() {
    for (let idx = next++; idx < files.length; idx = next++) {
      const path = files[idx];
      const rel = relative(join(ROOT, "testfiles"), path).replaceAll("\\", "/");
      const bytes = statSync(path).size;
      const proc = Bun.spawn({
        cmd: [TEST, "-dump-features", path],
        stdout: "pipe",
        stderr: "pipe",
      });
      const out = await new Response(proc.stdout).text();
      const err = await new Response(proc.stderr).text();
      const code = await proc.exited;
      let feat: FileFeat;
      if (code !== 0) {
        feat = {
          path,
          rel,
          bytes,
          pages: [],
          pagesCount: 0,
          container: "error",
          ncompDjvi: 0,
          incl: false,
          inclDjbz: false,
          inlineDjbz: false,
          outline: false,
          text: false,
          annot: false,
          links: false,
          typeBitonal: false,
          typePhoto: false,
          typeCompound: false,
          kindMask: false,
          kindBg: false,
          kindOther: false,
          fgbz: false,
          fg44: false,
          bgjp: false,
          fgjp: false,
          rotNonzero: false,
          totalRenderMs: 0,
          tags: [],
          error: err.trim() || `exit ${code}`,
        };
      } else {
        const parsed = parseDump(out);
        feat = { path, rel, bytes, ...parsed, tags: [] };
        feat.tags = featureTags(feat);
      }
      results[idx] = feat;
      done++;
      if (done % 25 === 0 || done === files.length) {
        console.log(`[${done}/${files.length}] ${basename(path)}`);
      }
    }
  }

  await Promise.all(Array.from({ length: workers }, () => worker()));
  return results;
}

function copySubset(picked: FileFeat[]) {
  mkdirSync(SUBSET_DIR, { recursive: true });
  for (const name of readdirSync(SUBSET_DIR)) {
    if (name.toLowerCase().endsWith(".djvu")) {
      const p = join(SUBSET_DIR, name);
      if (statSync(p).isFile()) unlinkSync(p);
    }
  }
  for (const f of picked) {
    const dest = join(SUBSET_DIR, basename(f.path));
    copyFileSync(f.path, dest);
  }
  writeFileSync(
    join(SUBSET_DIR, "README.txt"),
    "Curated subset copied by cmd/dump_features.ts --pick\n" +
      picked.map((f) => f.rel).join("\n") +
      "\n",
  );
}

async function main() {
  const args = process.argv.slice(2).filter((a) => a !== "-clang");
  const pickOnly = args.includes("--pick-only");
  const pick = args.includes("--pick") || pickOnly;
  const cpuArg = args.indexOf("-cpu");
  const workers = Math.max(
    1,
    cpuArg >= 0 ? parseInt(args[cpuArg + 1]) || 1 : cpus().length,
  );
  const dirArg = args.find((a) => !a.startsWith("-") && a !== args[cpuArg + 1]);
  const scanDir = dirArg ?? DEFAULT_DIR;

  let all: FileFeat[];
  if (pickOnly && existsSync(OUT_JSONL)) {
    all = readFileSync(OUT_JSONL, "utf8")
      .split(/\r?\n/)
      .filter((l) => l.length)
      .map((l) => JSON.parse(l) as FileFeat);
    console.log(`loaded ${all.length} records from ${OUT_JSONL}`);
  } else {
    if (!existsSync(scanDir)) {
      console.error(`directory not found: ${scanDir}`);
      process.exit(1);
    }
    all = await dumpDir(scanDir, workers);
    writeFileSync(
      OUT_JSONL,
      all.map((f) => JSON.stringify(f)).join("\n") + "\n",
    );
    console.log(`wrote ${OUT_JSONL}`);
  }

  if (pick) {
    const picked = pickSubset(all);
    writeSummary(all, picked);
    console.log(`wrote ${OUT_SUMMARY}`);
    copySubset(picked);
    console.log(`copied ${picked.length} files to ${SUBSET_DIR}`);
    const ms = picked.reduce((s, f) => s + f.totalRenderMs, 0);
    console.log(
      `subset: ${picked.length} files, ${picked.reduce((s, f) => s + f.pagesCount, 0)} pages, ~${(ms / 1000).toFixed(1)}s render`,
    );
  } else {
    writeSummary(all, null);
    console.log(`wrote ${OUT_SUMMARY}`);
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});