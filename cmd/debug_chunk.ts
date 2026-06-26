#!/usr/bin/env bun
// Run one -verify-render chunk with live RSS monitoring + djvu_test mem stderr.
//
//   bun cmd/debug_chunk.ts [lo] [hi]
//   bun cmd/debug_chunk.ts 1 16
//
// Env forwarded: DJVU_VERIFY_MEM_MB (default 4096 in harness), DJVU_VERIFY_OURS_ONLY, etc.
import { join, dirname } from "path";
import { mkdirSync } from "fs";
import { dlopen, FFIType, ptr } from "bun:ffi";
import { fmtBytes } from "./win_proc_mem";
import { build, defaultUseClang } from "./build";

const ROOT = dirname(import.meta.dir);
const FILE = join(
  ROOT,
  "testfiles/full/Popov E'.V. Obshchenie s E'VM na estestvennom jazyke (Nauka, 1982)(ru)(T)(360s)_CsAi_.djvu",
);
const lo = parseInt(process.argv[2] ?? "1", 10);
const hi = parseInt(process.argv[3] ?? "16", 10);
const memLimitMb = parseInt(process.env.DJVU_VERIFY_MEM_MB ?? "4096", 10);

const k32 = dlopen("kernel32.dll", {
  OpenProcess: { args: [FFIType.u32, FFIType.u32, FFIType.u32], returns: FFIType.u64 },
  CloseHandle: { args: [FFIType.u64], returns: FFIType.u32 },
});
const psapi = dlopen("psapi.dll", {
  GetProcessMemoryInfo: { args: [FFIType.u64, FFIType.ptr, FFIType.u32], returns: FFIType.u32 },
});

const PMC_COMMIT_OFF = 64;

function processMem(pid: number): { ws: number; commit: number } {
  const h = k32.symbols.OpenProcess(0x1000, 0, pid);
  if (!h || h === BigInt(-1)) return { ws: 0, commit: 0 };
  const mem = new Uint8Array(72);
  const mv = new DataView(mem.buffer);
  mv.setUint32(0, 72, true);
  psapi.symbols.GetProcessMemoryInfo(h, ptr(mem), 72);
  k32.symbols.CloseHandle(h);
  return {
    ws: Number(mv.getBigUint64(16, true)),
    commit: Number(mv.getBigUint64(PMC_COMMIT_OFF, true)),
  };
}

const TEST = await build(defaultUseClang);
const DIFF = join(ROOT, "verify_diffs", "debug_chunk");
mkdirSync(DIFF, { recursive: true });

console.log(`chunk p${lo}-${hi} mem_limit=${memLimitMb} MB file=${FILE}`);

const proc = Bun.spawn({
  cmd: [TEST, "-verify-render", "-diffdir", DIFF, FILE],
  stdout: "pipe",
  stderr: "pipe",
  env: {
    ...process.env,
    DJVU_VERIFY_LO: String(lo),
    DJVU_VERIFY_HI: String(hi),
    DJVU_VERIFY_MEM_MB: String(memLimitMb),
  },
});

const pid = proc.pid!;
let peakWs = 0;
let peakCommit = 0;
let killed = false;

const poll = setInterval(() => {
  const { ws, commit } = processMem(pid);
  if (ws > peakWs) peakWs = ws;
  if (commit > peakCommit) peakCommit = commit;
  const lim = memLimitMb * 1024 ** 2;
  if ((ws > lim || commit > lim) && !killed) {
    killed = true;
    console.error(
      `\n[monitor] ws=${fmtBytes(ws)} commit=${fmtBytes(commit)} > ${memLimitMb} MB — killing pid ${pid}`,
    );
    proc.kill();
  }
}, 100);

async function drain(stream: ReadableStream<Uint8Array>, tag: string) {
  const dec = new TextDecoder();
  let carry = "";
  for await (const chunk of stream) {
    carry += dec.decode(chunk);
    const lines = carry.split(/\r?\n/);
    carry = lines.pop() ?? "";
    for (const line of lines) {
      if (!line) continue;
      const { ws, commit } = processMem(pid);
      if (line.startsWith("mem") || line.startsWith("mem_dbg"))
        console.error(`[${tag}] ${line}`);
      else if (line.startsWith("render\t"))
        console.log(`[${tag}] ${line} (ws ${fmtBytes(ws)} commit ${fmtBytes(commit)})`);
    }
  }
  if (carry) console.error(`[${tag}] ${carry}`);
}

await Promise.all([drain(proc.stdout, "out"), drain(proc.stderr, "err")]);
clearInterval(poll);
const code = await proc.exited;

console.log(
  `exit=${code} peak_ws=${fmtBytes(peakWs)} peak_commit=${fmtBytes(peakCommit)} killed_by_monitor=${killed}`,
);
process.exit(code === 3 ? 3 : code !== 0 && code !== 1 ? 1 : 0);