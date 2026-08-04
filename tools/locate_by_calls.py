#!/usr/bin/env python3
"""Locate host functions that byte-patterns cannot reach, by CALL ALIGNMENT.

Some functions compile differently enough between our symbol build and the
official one that no prologue pattern matches (measured: DiffDataSets::
MaterializeSet and ::ZeroVertDiff). But they are CALLED from a function we did
locate, and a call sequence is far more stable than the code around it, because
it follows the source statement order.

So: read the ordered list of callees out of our object file, where the
relocations name every target exactly; read the ordered list of call targets out
of the official binary at the address we already trust; and align the two. Every
candidate must also begin a real function according to .pdata, which is what
keeps a stray E8 byte inside some operand from being mistaken for a call.

Reports only what it can align unambiguously. Anything else is left unlocated -
a wrong address here crashes the user's OutfitStudio.
"""
from __future__ import annotations
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from locate_host_funcs import Coff, pe_text, pdata_starts   # noqa: E402

IMAGE_REL_AMD64_REL32 = 0x0004


def obj_call_targets(c: Coff, secidx: int, start: int, end: int):
    """(offset, symbol name) for each REL32 relocation that follows an E8."""
    s = c.sections[secidx]
    code = c.d[s["rawptr"]:s["rawptr"] + s["rawsize"]]
    out = []
    for k in range(s["nrel"]):
        va, symidx, typ = struct.unpack_from("<IIH", c.d, s["relptr"] + k * 10)
        if typ != IMAGE_REL_AMD64_REL32:
            continue
        if not (start <= va - 1 < end):
            continue
        if code[va - 1] != 0xE8:          # relocation is the displacement of a call
            continue
        off = c.symptr + symidx * c.symsize
        raw = c.d[off:off + 8]
        out.append((va - 1 - start, c._sym_name(raw)))
    out.sort()
    return out


def exe_call_targets(exe: bytes, text, begin_rva: int, end_rva: int, starts: set):
    """(offset, target rva) for each E8 whose destination begins a function."""
    t = next((x for x in text if x["vaddr"] <= begin_rva < x["vaddr"] + x["size"]), None)
    if t is None:
        return []
    base = t["rawptr"] + (begin_rva - t["vaddr"])
    n = end_rva - begin_rva
    body = exe[base:base + n]
    out = []
    i = 0
    while i < len(body) - 5:
        if body[i] == 0xE8:
            disp = struct.unpack_from("<i", body, i + 1)[0]
            tgt = begin_rva + i + 5 + disp
            if tgt in starts:
                out.append((i, tgt))
                i += 5
                continue
        i += 1
    return out


def func_extent(exe: bytes, rva: int):
    """(begin, end) for the .pdata entry that starts at rva."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    magic = struct.unpack_from("<H", exe, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20B else 96)
    prva, psize = struct.unpack_from("<II", exe, dd + 3 * 8)
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    optsize = struct.unpack_from("<H", exe, pe + 20)[0]
    off = pe + 24 + optsize
    secs = []
    for _ in range(nsec):
        _n, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", exe, off)
        secs.append((vaddr, vsize or rawsize, rawptr))
        off += 40
    fo = next((rp + (prva - va) for va, vs, rp in secs if va <= prva < va + vs), None)
    if fo is None:
        return None
    for k in range(psize // 12):
        b, e, _u = struct.unpack_from("<III", exe, fo + k * 12)
        if b == rva:
            return b, e
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obj", required=True, help="object holding the anchor function")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--anchor-sym", required=True, help="mangled-name fragment of the anchor")
    ap.add_argument("--anchor-rva", required=True, help="its already-trusted RVA, hex")
    ap.add_argument("--want", action="append", default=[],
                    help="mangled-name fragment(s) to resolve")
    a = ap.parse_args()

    exe = Path(a.exe).read_bytes()
    text, _base = pe_text(exe)
    starts = pdata_starts(exe)
    rva = int(a.anchor_rva, 16)
    ext = func_extent(exe, rva)
    if not ext:
        print(f"ancora 0x{rva:x} nao consta em .pdata - abortado")
        return 1
    begin, end = ext
    print(f"ancora 0x{begin:x}..0x{end:x}  ({end - begin} bytes)")

    c = Coff(Path(a.obj).read_bytes())
    sym = c.find_symbol(a.anchor_sym)
    if not sym:
        print("simbolo ancora nao encontrado no obj")
        return 1
    s = c.sections[sym["section"]]
    ours = obj_call_targets(c, sym["section"], sym["value"], s["rawsize"])
    theirs = exe_call_targets(exe, text, begin, end, starts)
    print(f"chamadas: no nosso obj {len(ours)}, no exe oficial {len(theirs)}")
    if len(ours) != len(theirs):
        print("!! contagens diferem - alinhamento posicional nao e' confiavel")
        print("   (inlining diferente entre os builds)")
    for i, (off, name) in enumerate(ours):
        short = name.split("@@")[0].lstrip("?")
        hit = ""
        if len(ours) == len(theirs):
            hit = f"  -> 0x{theirs[i][1]:x}"
        want = any(w in name for w in a.want)
        mark = "  <==" if want else ""
        print(f"  [{i:2d}] +0x{off:04x}  {short[:52]:52s}{hit}{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
