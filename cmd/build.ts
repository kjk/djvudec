// build.ts -- build & test driver for the djvu C port (run with `bun cmd/build.ts`).
//
//   bun cmd/build.ts          build ref tools (if needed) + the C library/harness
//   bun cmd/build.ts ref      (re)build the DjVuLibre reference tools
//   bun cmd/build.ts test     build, then verify against ddjvu/djvutxt
//
import { $ } from "bun";
import { existsSync, readdirSync, mkdirSync } from "fs";

const ROOT = `${import.meta.dir}/..`;
const DJVULIBRE = "C:/Users/kjk/src/DjVuLibre";
const SPECS = `${ROOT}/testfiles/djvunet`;
const REF = `${ROOT}/ref_build`;

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

function specFiles(): string[] {
  return readdirSync(SPECS).filter((f) => f.toLowerCase().endsWith(".djvu"));
}

// Build ddjvu.exe / djvutxt.exe from DjVuLibre (static, decode oracle).
async function buildRef() {
  mkdirSync(REF, { recursive: true });
  const common =
    `-std=c++14 -w -O1 -DHAVE_NAMESPACES -DWIN32 -D_CRT_SECURE_NO_WARNINGS ` +
    `-DDJVUAPI_EXPORT -DDDJVUAPI_EXPORT -DMINILISPAPI_EXPORT ` +
    `-I${DJVULIBRE} -I${DJVULIBRE}/libdjvu`;
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

// Build the C library + test harness.
async function build() {
  console.log("building djvu_test...");
  await $`clang -std=c11 -g -O1 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Iinclude -Isrc ${{ raw: SRCS.join(" ") }} test/djvu_test.c -o djvu_test.exe`.cwd(ROOT);
  console.log("built djvu_test.exe");
}

// Verify page info (count + dimensions) against ddjvu, and text against djvutxt.
async function test() {
  await buildRef();
  await build();
  let pass = 0, fail = 0;
  for (const f of specFiles()) {
    const path = `${SPECS}/${f}`;
    // our page info
    const mine = await $`./djvu_test.exe -info ${path}`.cwd(ROOT).quiet().text();
    const myPages = parseInt((mine.match(/pages: (\d+)/) || [])[1] || "-1");
    // reference: render page 1 to pgm and read its dimensions
    const tmp = `${REF}/_ref.pgm`;
    await $`${REF}/ddjvu.exe -format=pgm -page=1 ${path} ${tmp}`.quiet().nothrow();
    const head = await $`head -c 32 ${tmp}`.quiet().text().catch(() => "");
    const dims = head.split(/\s+/).slice(1, 3).join("x");
    const myP1 = (mine.match(/page 1: (\d+x\d+)/) || [])[1] || "?";
    const ok = myP1 === dims;
    console.log(`${ok ? "PASS" : "FAIL"} ${f}: pages=${myPages} p1=${myP1} ref=${dims}`);
    ok ? pass++ : fail++;
  }
  console.log(`\npage-info: ${pass} pass, ${fail} fail`);

  // render verification (pure JB2-mask pages must match ddjvu byte-for-byte)
  console.log("\nrender verification:");
  await $`bun cmd/verify.ts`.cwd(ROOT).nothrow();
}

const cmd = process.argv[2];
if (cmd === "ref") await buildRef();
else if (cmd === "test") await test();
else { await buildRef(); await build(); }
