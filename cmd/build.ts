// build.ts -- build driver for the djvu C port (run with `bun cmd/build.ts`).
//
//   bun cmd/build.ts          fetch deps + build ref tools + the C library/harness
//   bun cmd/build.ts ref      (re)build the DjVuLibre reference tools
//
// Verification lives in verify.ts, which imports buildRef()/build() from here
// and drives them (build first, then verify) -- run `bun cmd/verify.ts`.
import { $ } from "bun";
import { existsSync, mkdirSync } from "fs";
import { getDeps } from "./get-deps";

// Forward slashes: Bun's shell treats backslashes as escapes, which breaks the
// *.cpp / *.o globs below (import.meta.dir is backslashed on Windows).
const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const DJVULIBRE = `${ROOT}/../DjVuLibre`; // sibling checkout (see get-deps.ts)
const REF = `${ROOT}/ref_build`;
const LIBDJVU = `${REF}/libdjvu.lib`; // cached static lib (for djvu_test -bench)
const OBJDIR = `${REF}/djvuobj`;

// DjVuLibre C++ compile flags (static link, no dll import/export indirection).
// -O3: both sides are built at max optimization so `-bench` is a fair compare.
const DJVU_CXXFLAGS =
  `-std=c++14 -w -O3 -DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
  `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
  `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;

const SRCS = [
  "src/zptable.c",
  "src/zpcodec.c",
  "src/bzz.c",
  "src/bitmap.c",
  "src/jb2.c",
  "src/iw44_zigzag.c",
  "src/iw44.c",
  "src/compose.c",
  "src/document.c",
  "src/render.c",
  "src/text.c",
  "src/outline.c",
  "src/annot.c",
];

// Build ddjvu.exe / djvutxt.exe from DjVuLibre (static, decode oracle).
export async function buildRef() {
  mkdirSync(REF, { recursive: true });
  const common = DJVU_CXXFLAGS;
  const libsrc = `${DJVULIBRE}/libdjvu/*.cpp`;
  for (const tool of ["ddjvu", "djvutxt", "bzz", "djvused"]) {
    const exe = `${REF}/${tool}.exe`;
    if (existsSync(exe)) continue;
    console.log(`building ref tool ${tool}...`);
    await $`clang++ ${{ raw: common }} ${{ raw: libsrc }} ${DJVULIBRE}/tools/${tool}.cpp -ladvapi32 -o ${exe}`;
  }
  // iw44ref: decodes a FORM:PM44 directly via IW44Image (no gamma pipeline),
  // used to verify the IW44 codec in isolation. Source lives in test/.
  if (!existsSync(`${REF}/iw44ref.exe`)) {
    console.log("building ref tool iw44ref...");
    await $`clang++ ${{ raw: common }} ${{ raw: libsrc }} ${ROOT}/test/iw44ref.cpp -ladvapi32 -o ${REF}/iw44ref.exe`;
  }
  // jb2ref: decodes a raw Sjbz with DjVuLibre's JB2Image (dumps blits / mask),
  // used to verify the JB2 codec in isolation.
  if (!existsSync(`${REF}/jb2ref.exe`)) {
    console.log("building ref tool jb2ref...");
    await $`clang++ ${{ raw: common }} ${{ raw: libsrc }} ${ROOT}/test/jb2ref.cpp -ladvapi32 -o ${REF}/jb2ref.exe`;
  }
  console.log("ref tools ready");
}

// Compile all of libdjvu into a cached static lib (one-time, slow) so djvu_test
// can link DjVuLibre's decoder for `-bench` without recompiling it every build.
export async function buildLibDjvu() {
  if (existsSync(LIBDJVU)) return;
  console.log("building libdjvu.lib (one-time, slow)...");
  mkdirSync(OBJDIR, { recursive: true });
  await $`clang++ ${{ raw: DJVU_CXXFLAGS }} -c ${{ raw: `${DJVULIBRE}/libdjvu/*.cpp` }}`.cwd(OBJDIR);
  await $`llvm-lib /out:${LIBDJVU} ${{ raw: "*.o" }}`.cwd(OBJDIR);
  console.log("built libdjvu.lib");
}

// Build the C library + test harness. djvu_test links the DjVuLibre timing shim
// (bench_ddjvu.cpp) + libdjvu.lib so `djvu_test -bench` can compare decode speed.
export async function build() {
  await buildLibDjvu();
  console.log("building djvu_test...");
  const objs = [
    ...SRCS.map((s) => s.replace(/^src\//, "").replace(/\.c$/, ".o")),
    "djvu_test.o",
  ];
  // 1. our C sources + harness -> objects (compiled as C, -O3 for benchmarking)
  await $`clang -std=c11 -g -O3 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Isrc -c ${{ raw: SRCS.join(" ") }} test/djvu_test.c`.cwd(ROOT);
  // 2. DjVuLibre timing shim -> object (compiled as C++)
  await $`clang++ ${{ raw: DJVU_CXXFLAGS }} -c test/bench_ddjvu.cpp`.cwd(ROOT);
  // 3. link with clang++ (C++ runtime) against libdjvu.lib
  await $`clang++ ${{ raw: objs.join(" ") }} bench_ddjvu.o ${LIBDJVU} -ladvapi32 -o djvu_test.exe`.cwd(ROOT);
  console.log("built djvu_test.exe");
}

if (import.meta.main) {
  await getDeps();
  const cmd = process.argv[2];
  if (cmd === "ref") await buildRef();
  else {
    await buildRef();
    await build();
  }
}
