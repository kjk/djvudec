// Default test/bench corpus: testfiles/subset (curated). Pass -full for testfiles/full.
// DJVU_SPECS overrides both.
import { join } from "path";

export function corpusDir(root: string, argv: string[] = process.argv): string {
  if (process.env.DJVU_SPECS) return process.env.DJVU_SPECS;
  const name = argv.includes("-full") ? "full" : "subset";
  return join(root, "testfiles", name);
}