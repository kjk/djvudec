#!/usr/bin/env bun
// profile_render.ts -- collect a single-page render profile.
//
//   bun cmd/profile_render.ts file.djvu -p 39
//   bun cmd/profile_render.ts file.djvu -p 39 -reps 180 -seconds 8
//   bun cmd/profile_render.ts file.djvu -p 39 -tool xctrace
//   bun cmd/profile_render.ts file.djvu -p 39 -variant before
//
// The default uses macOS `sample` and writes a text call tree under out/profile/.
// `-tool xctrace` records an Instruments Time Profiler trace when full Xcode is
// installed and `xcrun xctrace` is available.
import { existsSync, mkdirSync } from "fs";
import { basename } from "path";
import { defaultUseClang } from "./build";
import { benchTarget, buildLibTool, DUMP_TARGET } from "./build_lib";

type Tool = "sample" | "xctrace";
type Variant = "dump" | "before" | "after";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");

function usage(): never {
  console.error(
    "usage: bun cmd/profile_render.ts file.djvu -p N " +
      "[-reps N] [-warm N] [-seconds N] [-tool sample|xctrace] " +
      "[-variant dump|before|after] [-clang]",
  );
  process.exit(2);
}

function argValue(args: string[], name: string): string | null {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : null;
}

function intArg(args: string[], name: string, def: number): number {
  const v = argValue(args, name);
  if (v == null) return def;
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? n : def;
}

function inputFileArg(args: string[]): string | null {
  const valueFlags = new Set([
    "-p", "-page", "-reps", "-warm", "-seconds", "-tool", "-variant",
  ]);
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (valueFlags.has(a)) {
      i++;
      continue;
    }
    if (!a.startsWith("-")) return a;
  }
  return null;
}

async function textOf(stream: ReadableStream<Uint8Array> | null): Promise<string> {
  return stream ? new Response(stream).text() : "";
}

async function sleep(ms: number): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, ms));
}

async function commandExists(cmd: string[]): Promise<boolean> {
  const p = Bun.spawn({ cmd, stdout: "ignore", stderr: "ignore" });
  return (await p.exited) === 0;
}

async function buildTarget(variant: Variant, useClang: boolean): Promise<string> {
  if (variant === "before" || variant === "after")
    return buildLibTool(benchTarget(variant), useClang);
  return buildLibTool(DUMP_TARGET, useClang);
}

async function runSample(
  exe: string,
  benchArgs: string[],
  seconds: number,
  outPath: string,
): Promise<number> {
  const target = Bun.spawn({
    cmd: [exe, ...benchArgs],
    stdout: "pipe",
    stderr: "pipe",
  });

  await sleep(250);
  const sampler = Bun.spawn({
    cmd: ["/usr/bin/sample", String(target.pid), String(seconds), "-file", outPath],
    stdout: "pipe",
    stderr: "pipe",
  });

  const [sampleOut, sampleErr, sampleCode] = await Promise.all([
    textOf(sampler.stdout),
    textOf(sampler.stderr),
    sampler.exited,
  ]);
  const [targetOut, targetErr, targetCode] = await Promise.all([
    textOf(target.stdout),
    textOf(target.stderr),
    target.exited,
  ]);

  if (targetOut.trim()) console.log(targetOut.trim());
  if (targetErr.trim()) console.error(targetErr.trim());
  if (sampleOut.trim()) console.log(sampleOut.trim());
  if (sampleErr.trim()) console.error(sampleErr.trim());
  if (targetCode !== 0) return targetCode;
  return sampleCode;
}

async function runXctrace(
  exe: string,
  benchArgs: string[],
  seconds: number,
  outPath: string,
): Promise<number> {
  if (!(await commandExists(["xcrun", "-find", "xctrace"]))) {
    console.error("xcrun xctrace is not available; install full Xcode or use -tool sample");
    return 1;
  }
  const p = Bun.spawn({
    cmd: [
      "xcrun", "xctrace", "record",
      "--template", "Time Profiler",
      "--time-limit", `${seconds}s`,
      "--output", outPath,
      "--launch", "--",
      exe,
      ...benchArgs,
    ],
    stdout: "inherit",
    stderr: "inherit",
  });
  return p.exited;
}

async function main(): Promise<number> {
  const args = process.argv.slice(2);
  const file = inputFileArg(args);
  const page = intArg(args, "-p", intArg(args, "-page", 0));
  const reps = intArg(args, "-reps", 120);
  const warm = intArg(args, "-warm", 1);
  const seconds = intArg(args, "-seconds", 8);
  const tool = (argValue(args, "-tool") ?? "sample") as Tool;
  const variant = (argValue(args, "-variant") ?? "dump") as Variant;
  const useClang = args.includes("-clang") || defaultUseClang;

  if (!file || page <= 0) usage();
  if (!existsSync(file)) {
    console.error(`no such file: ${file}`);
    return 1;
  }
  if (tool !== "sample" && tool !== "xctrace") usage();
  if (variant !== "dump" && variant !== "before" && variant !== "after") usage();
  if (process.platform !== "darwin") {
    console.error("profile_render.ts currently supports macOS profiling tools only");
    return 1;
  }
  if (tool === "sample" && !existsSync("/usr/bin/sample")) {
    console.error("/usr/bin/sample is not available");
    return 1;
  }

  const exe = await buildTarget(variant, useClang);
  const outDir = `${ROOT}/out/profile`;
  mkdirSync(outDir, { recursive: true });
  const stem = `${basename(file).replace(/[^A-Za-z0-9_.-]/g, "_")}_p${page}_${Date.now()}`;
  const outPath = tool === "xctrace"
    ? `${outDir}/${stem}.trace`
    : `${outDir}/${stem}.sample.txt`;
  const benchArgs = [
    "-bench-render",
    "-warm", String(warm),
    "-reps", String(reps),
    "-page", String(page),
    file,
  ];

  console.log(`exe: ${exe}`);
  console.log(`profile: ${outPath}`);
  console.log(`render args: ${benchArgs.join(" ")}`);

  const code = tool === "xctrace"
    ? await runXctrace(exe, benchArgs, seconds, outPath)
    : await runSample(exe, benchArgs, seconds, outPath);
  if (code === 0)
    console.log(`wrote ${outPath}`);
  return code;
}

main().then((code) => process.exit(code)).catch((e) => {
  console.error(e);
  process.exit(1);
});
