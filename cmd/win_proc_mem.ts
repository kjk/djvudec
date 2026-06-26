// Windows process memory enumeration via bun:ffi (kernel32 + psapi).
// Used by tests.ts to detect runaway djvu_test_* processes after each subprocess exits.

import { dlopen, FFIType, ptr } from "bun:ffi";

const TH32CS_SNAPPROCESS = 0x00000002;
const INVALID_HANDLE = BigInt(-1);
const PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
const PROCESS_TERMINATE = 0x0001;

// PROCESSENTRY32W (win64): szExeFile at byte offset 44.
const PE32W_SIZE = 568;
const PE32W_NAME_OFF = 44;

// PROCESS_MEMORY_COUNTERS: WorkingSetSize at offset 16 (win64).
const PMC_SIZE = 72;
const PMC_WORKING_SET_OFF = 16;

export const DJVU_TEST_MEM_LIMIT = 8 * 1024 ** 3;

export type DjvuTestProc = { pid: number; name: string; workingSetBytes: number };

type WinApis = {
  enumDjvuTestProcs(): DjvuTestProc[];
  killDjvuTestProcs(): void;
};

let apis: WinApis | null = null;

function winApis(): WinApis | null {
  if (process.platform !== "win32") return null;
  if (apis) return apis;

  const k32 = dlopen("kernel32.dll", {
    CreateToolhelp32Snapshot: { args: [FFIType.u32, FFIType.u32], returns: FFIType.u64 },
    Process32FirstW: { args: [FFIType.u64, FFIType.ptr], returns: FFIType.u32 },
    Process32NextW: { args: [FFIType.u64, FFIType.ptr], returns: FFIType.u32 },
    CloseHandle: { args: [FFIType.u64], returns: FFIType.u32 },
    OpenProcess: { args: [FFIType.u32, FFIType.u32, FFIType.u32], returns: FFIType.u64 },
    TerminateProcess: { args: [FFIType.u64, FFIType.u32], returns: FFIType.u32 },
  });

  const psapi = dlopen("psapi.dll", {
    GetProcessMemoryInfo: { args: [FFIType.u64, FFIType.ptr, FFIType.u32], returns: FFIType.u32 },
  });

  const pe = new Uint8Array(PE32W_SIZE);
  const peView = new DataView(pe.buffer);
  const mem = new Uint8Array(PMC_SIZE);
  const memView = new DataView(mem.buffer);

  function readExeName(): string {
    let name = "";
    for (let i = 0; i < 260; i++) {
      const c = peView.getUint16(PE32W_NAME_OFF + i * 2, true);
      if (c === 0) break;
      name += String.fromCharCode(c);
    }
    return name;
  }

  function processWorkingSet(pid: number): number | null {
    const h = k32.symbols.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
    if (!h || h === INVALID_HANDLE) return null;
    memView.setUint32(0, PMC_SIZE, true);
    const ok = psapi.symbols.GetProcessMemoryInfo(h, ptr(mem), PMC_SIZE);
    k32.symbols.CloseHandle(h);
    if (!ok) return null;
    return Number(memView.getBigUint64(PMC_WORKING_SET_OFF, true));
  }

  apis = {
    enumDjvuTestProcs() {
      const out: DjvuTestProc[] = [];
      peView.setUint32(0, PE32W_SIZE, true);
      const snap = k32.symbols.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (!snap || snap === INVALID_HANDLE) return out;

      let more = k32.symbols.Process32FirstW(snap, ptr(pe));
      while (more) {
        const name = readExeName();
        if (/^djvu_test_/i.test(name)) {
          const pid = peView.getUint32(8, true);
          const ws = processWorkingSet(pid);
          if (ws !== null) out.push({ pid, name, workingSetBytes: ws });
        }
        more = k32.symbols.Process32NextW(snap, ptr(pe));
      }
      k32.symbols.CloseHandle(snap);
      return out;
    },

    killDjvuTestProcs() {
      peView.setUint32(0, PE32W_SIZE, true);
      const snap = k32.symbols.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (!snap || snap === INVALID_HANDLE) return;

      let more = k32.symbols.Process32FirstW(snap, ptr(pe));
      while (more) {
        const name = readExeName();
        if (/^djvu_test_/i.test(name)) {
          const pid = peView.getUint32(8, true);
          const h = k32.symbols.OpenProcess(PROCESS_TERMINATE, 0, pid);
          if (h && h !== INVALID_HANDLE) {
            k32.symbols.TerminateProcess(h, 1);
            k32.symbols.CloseHandle(h);
          }
        }
        more = k32.symbols.Process32NextW(snap, ptr(pe));
      }
      k32.symbols.CloseHandle(snap);
    },
  };

  return apis;
}

/** Human-readable byte size, e.g. 8.5 GB. */
export function fmtBytes(n: number): string {
  if (n >= 1024 ** 3) return `${(n / 1024 ** 3).toFixed(2)} GB`;
  if (n >= 1024 ** 2) return `${(n / 1024 ** 2).toFixed(1)} MB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${n} B`;
}

/** Record pid → file as soon as djvu_test is spawned (kept for the whole test run). */
export function trackDjvuTestProc(
  proc: Subprocess,
  file: string,
  pidToFile: Map<number, string>,
): void {
  if (proc.pid) pidToFile.set(proc.pid, file);
}

/**
 * After a djvu_test subprocess exits, scan for any djvu_test_* process over the
 * memory limit. Returns the file path associated with the offender, or null.
 */
export function checkDjvuTestMemory(
  pidToFile: Map<number, string>,
  limitBytes = DJVU_TEST_MEM_LIMIT,
): { file: string; pid: number; name: string; bytes: number } | null {
  const w = winApis();
  if (!w) return null;

  for (const proc of w.enumDjvuTestProcs()) {
    if (proc.workingSetBytes <= limitBytes) continue;
    const file = pidToFile.get(proc.pid) ?? "(unknown file)";
    return { file, pid: proc.pid, name: proc.name, bytes: proc.workingSetBytes };
  }
  return null;
}

/** Await subprocess exit, then check for runaway djvu_test_* memory. */
export async function awaitDjvuTestProc(
  proc: Subprocess,
  pidToFile: Map<number, string>,
): Promise<number> {
  const code = await proc.exited;

  const hit = checkDjvuTestMemory(pidToFile, DJVU_TEST_MEM_LIMIT);
  if (hit) {
    console.error(
      `MEMORY LIMIT: ${hit.name} (pid ${hit.pid}) using ${fmtBytes(hit.bytes)} ` +
        `(limit ${fmtBytes(DJVU_TEST_MEM_LIMIT)})`,
    );
    console.error(`testing file: ${hit.file}`);
    killAllDjvuTestProcs();
    process.exit(2);
  }
  return code;
}

/** Kill every djvu_test_* process on the system. */
export function killAllDjvuTestProcs(): void {
  winApis()?.killDjvuTestProcs();
}