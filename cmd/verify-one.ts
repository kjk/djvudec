import { writeFileSync } from "fs";
import { join, dirname } from "path";
import { build } from "./build";
import { corpusSummary, selectFiles } from "./corpus";

const ROOT = dirname(import.meta.dir);
const [f] = selectFiles(
  `usage: bun cmd/verify-one.ts <file.djvu | -rand 1> [--bench] [--tofile] [--ours-only]

${corpusSummary()}`,
);
const TEST = await build(false);
const bench = process.argv.includes("--bench");
const DUMP = join(ROOT, "out/msvc/djvudec_dump.exe");
const toFile = process.argv.includes("--tofile") || process.argv.includes("--ours-only");
const oursOnly = process.argv.includes("--ours-only");
const proc = Bun.spawn({
  cmd: bench ? [DUMP, "-bench-render", f] : [TEST, "-verify-render", f],
  stdout: toFile ? "pipe" : "inherit",
  stderr: "inherit",
  env: oursOnly ? { ...process.env, DJVU_VERIFY_OURS_ONLY: "1" } : process.env,
});
const out = toFile ? await new Response(proc.stdout).text() : "";
const code = await proc.exited;
if (toFile) writeFileSync(join(ROOT, "out/verify_out.txt"), out);
const lines = out.split(/\r?\n/).filter((l) => (bench ? l.startsWith("p") : l.startsWith("render")) && l.length > 1);
console.log("exit", code, toFile ? `lines ${lines.length}` : "(stdout inherit)");
if (lines.length) console.log("last", lines[lines.length - 1]);
const summary = out.split(/\r?\n/).find((l) => l.startsWith("summary"));
if (summary) console.log(summary);
process.exit(code === 0 || code === 1 ? 0 : 1);