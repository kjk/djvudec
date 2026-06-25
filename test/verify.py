#!/usr/bin/env python3
"""Verify djvu_test render output against DjVuLibre's ddjvu, page by page.

Pages that are pure JB2 masks (Sjbz, no BG44/FG44 background) must match
ddjvu -format=pgm byte-for-byte. Pages with an IW44 background or color are
reported separately (those need the IW44 codec, not yet implemented)."""
import struct, subprocess, os, glob, sys, tempfile

SPECS = os.environ.get("DJVU_SPECS", r"C:\Users\kjk\src\DjvuNet\Specs")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RB = os.path.join(ROOT, "ref_build")
TEST = os.path.join(ROOT, "djvu_test.exe")
TMP = tempfile.gettempdir()

def page_offsets(data):
    p = 4 if data[:4] == b'AT&T' else 0
    if data[p:p+4] != b'FORM':
        return []
    ftype = data[p+8:p+12]
    if ftype == b'DJVU':
        return [p]
    end = p + 8 + struct.unpack('>I', data[p+4:p+8])[0]
    q = p + 12
    while q + 8 <= end:
        cid = data[q:q+4]; csz = struct.unpack('>I', data[q+4:q+8])[0]
        if cid == b'DIRM':
            d = q + 8; cnt = struct.unpack('>H', data[d+1:d+3])[0]
            offs = [struct.unpack('>I', data[d+3+i*4:d+7+i*4])[0] for i in range(cnt)]
            return [o for o in offs
                    if data[o:o+4] == b'FORM' and data[o+8:o+12] == b'DJVU']
        q += 8 + csz + (csz & 1)
    return []

def page_kind(data, off):
    """returns 'mask' (pure Sjbz), 'bg' (has IW44 background), or 'other'"""
    size = struct.unpack('>I', data[off+4:off+8])[0]
    end = off + 8 + size; p = off + 12
    bg = sj = False
    while p + 8 <= end:
        cid = data[p:p+4]; csz = struct.unpack('>I', data[p+4:p+8])[0]
        if cid in (b'BG44', b'FG44', b'BGjp', b'FGjp'): bg = True
        if cid == b'Sjbz': sj = True
        p += 8 + csz + (csz & 1)
    if bg: return 'bg'
    if sj: return 'mask'
    return 'other'

def render_ref(f, page, out):
    subprocess.run([os.path.join(RB, "ddjvu.exe"), "-format=pgm",
                    f"-page={page}", f, out], capture_output=True)

def render_mine(f, page, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([TEST, "-page", str(page), "-out", out, f], capture_output=True)

def main():
    m = mm = skip = 0
    bad = []
    for f in sorted(glob.glob(os.path.join(SPECS, "*.djvu"))):
        data = open(f, "rb").read()
        for i, o in enumerate(page_offsets(data)):
            if page_kind(data, o) != 'mask':
                skip += 1
                continue
            ref = os.path.join(TMP, "djref.pgm"); mine = os.path.join(TMP, "djmine.pgm")
            render_ref(f, i+1, ref)
            render_mine(f, i+1, mine)
            a = open(ref, "rb").read() if os.path.exists(ref) else b""
            b = open(mine, "rb").read() if os.path.exists(mine) else b""
            if a and a == b:
                m += 1
            else:
                mm += 1
                bad.append(f"{os.path.basename(f)} p{i+1} (ref={len(a)} mine={len(b)})")
    print(f"pure-mask pages: MATCH={m} MISMATCH={mm}; skipped(bg/color)={skip}")
    for x in bad[:50]:
        print("  MISMATCH", x)
    return 1 if mm else 0

if __name__ == "__main__":
    sys.exit(main())
