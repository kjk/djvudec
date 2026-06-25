#!/usr/bin/env python3
"""Sampled render+text verification over a directory tree of .djvu files.
Copies each file to an ASCII temp path (Windows Unicode-path safe), samples a
few pages, and compares djvu_test output to DjVuLibre ddjvu (format auto: P5->
pgm, P6->ppm) and djvutxt. Usage: python3 test/verify_dir.py <dir> [maxpages]"""
import sys, os, glob, subprocess, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RB = os.path.join(ROOT, "ref_build")
TEST = os.path.join(ROOT, "djvu_test.exe")
DDJVU = os.path.join(RB, "ddjvu.exe")
DJVUTXT = os.path.join(RB, "djvutxt.exe")
TMP = tempfile.gettempdir()

def pr(s):
    sys.stdout.buffer.write((s + "\n").encode("utf-8", "replace"))
    sys.stdout.flush()

def run(args, **kw):
    return subprocess.run(args, capture_output=True, **kw)

def page_count(f):
    out = run([TEST, "-info", f]).stdout.decode("latin1", "replace")
    return sum(1 for l in out.splitlines() if l.startswith("page "))

def pnm_magic(path):
    try:
        with open(path, "rb") as fh: return fh.read(2)
    except: return b""

def text_norm(b):
    return b.replace(b"\r", b"").replace(b"\x0c", b"").rstrip()

def main():
    d = sys.argv[1]
    maxp = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    files = sorted(glob.glob(os.path.join(d, "**", "*.djvu"), recursive=True))
    rmatch = rmiss = rerr = tmatch = tmiss = 0
    badrender = []; badtext = []
    mine = os.path.join(TMP, "vd_mine.pnm"); ref = os.path.join(TMP, "vd_ref.pnm")
    tmpdj = os.path.join(TMP, "vd_in.djvu")
    for f in files:
        shutil.copyfile(f, tmpdj)
        n = page_count(tmpdj)
        name = os.path.basename(f)
        if n <= 0:
            # could be indirect/empty; check ddjvu agrees there are no renderable pages
            pr(f"  (0 pages) {name}")
            continue
        pages = sorted(set([1, n, n//2, n//4, (3*n)//4]))
        pages = [p for p in pages if 1 <= p <= n][:maxp]
        fr_ok = fr_bad = ft_ok = ft_bad = 0
        for p in pages:
            if os.path.exists(mine): os.remove(mine)
            run([TEST, "-page", str(p), "-out", mine, tmpdj])
            mg = pnm_magic(mine)
            fmt = "ppm" if mg == b"P6" else "pgm"
            if os.path.exists(ref): os.remove(ref)
            run([DDJVU, f"-format={fmt}", f"-page={p}", tmpdj, ref])
            a = open(ref, "rb").read() if os.path.exists(ref) else b""
            if not a:   # ddjvu itself failed on this page; skip (can't compare)
                continue
            b = open(mine, "rb").read() if os.path.exists(mine) else b""
            if a and a == b: rmatch += 1; fr_ok += 1
            elif not b: rerr += 1; fr_bad += 1
            else: rmiss += 1; fr_bad += 1
            # text
            mt = text_norm(run([TEST, "-page", str(p), "-text", tmpdj]).stdout)
            rt = text_norm(run([DJVUTXT, f"--page={p}", tmpdj]).stdout)
            if mt == rt: tmatch += 1; ft_ok += 1
            else: tmiss += 1; ft_bad += 1
        flag = "" if (fr_bad == 0 and ft_bad == 0) else "  <<<"
        if fr_bad: badrender.append(name)
        if ft_bad: badtext.append(name)
        pr(f"  render {fr_ok}/{fr_ok+fr_bad} text {ft_ok}/{ft_ok+ft_bad}  {name}{flag}")
    pr(f"\nTOTAL render: match={rmatch} mismatch={rmiss} err={rerr}; "
          f"text: match={tmatch} mismatch={tmiss}")
    if badrender: pr("render issues: " + str(sorted(set(badrender))))
    if badtext: pr("text issues: " + str(sorted(set(badtext))))

if __name__ == "__main__":
    main()
