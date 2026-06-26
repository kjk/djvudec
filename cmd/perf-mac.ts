// perf-mac.ts -- profile Sumatra-style page render on macOS using `sample`.
//
//   bun cmd/perf-mac.ts <file.djvu> <page-no>
//
// Builds djvu_test with debug symbols (-g), runs -profile-sum in a tight render
// loop (one doc open, cached JB2 masks), records a `sample` stack trace, and
// writes the profile next to the binary under out/clang/perf/.
import { existsSync, mkdirSync } from "fs";
import { basename, dirname, join } from "path";
import { build, buildRef, defaultUseClang } from "./build";

const ROOT = dirname(import.meta.dir);
const SAMPLE_SEC = 10;
const READY = /profile-sum: ready/;

function usage(): never {
  console.error("usage: bun cmd/perf-mac.ts <file.djvu> <page-no>");
  process.exit(2);
}

if (process.platform !== "darwin") {
  console.error("perf-mac.ts requires macOS (`sample` profiler)");
  process.exit(1);
}

const file = process.argv[2];
const pageNo = Number.parseInt(process.argv[3] ?? "", 10);
if (!file || !Number.isFinite(pageNo) || pageNo < 1) usage();
if (!existsSync(file)) {
  console.error(`no such file: ${file}`);
  process.exit(1);
}

const useClang = defaultUseClang;
await buildRef();
const test = await build(useClang);

const perfDir = join(dirname(test), "perf");
mkdirSync(perfDir, { recursive: true });
const stem = basename(file).replace(/\.djvu$/i, "");
const outFile = join(perfDir, `${stem}-p${pageNo}.sample.txt`);

console.log(`profiling ${file} page ${pageNo} for ${SAMPLE_SEC}s -> ${outFile}`);

const proc = Bun.spawn({
  cmd: [test, "-profile-sum", "-page", String(pageNo), file],
  stdout: "inherit",
  stderr: "pipe",
});

let ready = false;
const dec = new TextDecoder();
const stderrReader = (async () => {
  const r = proc.stderr.getReader();
  for (;;) {
    const { value, done } = await r.read();
    if (done) break;
    const chunk = dec.decode(value);
    process.stderr.write(chunk);
    if (!ready && READY.test(chunk)) ready = true;
  }
})();

for (let i = 0; i < 200 && !ready; i++)
  await Bun.sleep(50);

if (!ready) {
  console.error("timed out waiting for profile-sum ready");
  proc.kill();
  await proc.exited;
  process.exit(1);
}

const sample = Bun.spawnSync({
  cmd: ["sample", String(proc.pid), String(SAMPLE_SEC), "-file", outFile],
  stdout: "inherit",
  stderr: "inherit",
});

proc.kill();
await Promise.all([proc.exited, stderrReader]);

if (sample.exitCode !== 0) {
  console.error(`sample failed (exit ${sample.exitCode})`);
  process.exit(sample.exitCode ?? 1);
}

console.log(`wrote ${outFile}`);
console.log(`open with: open -a \"Activity Monitor\" && File > Sample Process (or inspect ${outFile})`);