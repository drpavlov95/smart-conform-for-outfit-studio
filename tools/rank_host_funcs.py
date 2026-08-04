#!/usr/bin/env python3
"""Locate host functions by RANKING every real function in the binary.

locate_host_funcs.py demands an exact masked match, which finds a function only
when our build and the official one agreed on its prologue byte for byte. Two of
the five needed here never do (DiffDataSets::MaterializeSet and ::ZeroVertDiff).

But the binary declares where every function begins AND ends, in .pdata. So the
search does not have to be "find this needle anywhere in 18MB" - it can be "of
these 88,356 candidates, which one is this function?", which tolerates the
codegen differences an exact match cannot:

  * candidates are filtered by SIZE first (the same source compiled twice does
    not change length by much),
  * then scored on masked byte agreement over the prologue,
  * and the answer is only accepted when it beats the runner-up by a wide
    margin, because a close call is not a location, it is a guess.

Validated against the three functions locate_host_funcs.py already pinned by
exact match: they must come out on top here too, or this ranking is worthless.
"""
from __future__ import annotations
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from locate_host_funcs import Coff, pe_text, make_pattern, TARGETS   # noqa: E402


def pdata_functions(data: bytes):
    """[(begin_rva, end_rva)] for every function the binary declares."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20B else 96)
    prva, psize = struct.unpack_from("<II", data, dd + 3 * 8)
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    off = pe + 24 + optsize
    secs = []
    for _ in range(nsec):
        _n, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", data, off)
        secs.append((vaddr, vsize or rawsize, rawptr))
        off += 40
    fo = next((rp + (prva - va) for va, vs, rp in secs if va <= prva < va + vs), None)
    if fo is None:
        return [], secs
    out = []
    for k in range(psize // 12):
        b, e, _u = struct.unpack_from("<III", data, fo + k * 12)
        if b and e > b:
            out.append((b, e))
    return out, secs


def rva_to_file(secs, rva):
    for va, vs, rp in secs:
        if va <= rva < va + vs:
            return rp + (rva - va)
    return None


def score(cand: bytes, pat: bytes, mask: bytes) -> float:
    n = min(len(cand), len(pat))
    if n == 0:
        return 0.0
    tot = hit = 0
    for k in range(n):
        if not mask[k]:
            continue
        tot += 1
        if cand[k] == pat[k]:
            hit += 1
    return hit / tot if tot else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obj", action="append", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--len", type=int, default=64)
    ap.add_argument("--size-tol", type=float, default=0.45,
                    help="accepted |len difference| as a fraction of our length")
    ap.add_argument("--top", type=int, default=3)
    a = ap.parse_args()

    exe = Path(a.exe).read_bytes()
    funcs, secs = pdata_functions(exe)
    print(f"exe {len(exe):,} bytes   funcoes declaradas em .pdata: {len(funcs):,}")

    objs = [Coff(Path(p).read_bytes()) for p in a.obj]
    for label, needle in TARGETS:
        found = None
        for c in objs:
            s = c.find_symbol(needle)
            if s:
                found = (c, s)
                break
        if not found:
            print(f"\n{label}: simbolo ausente nos .obj")
            continue
        c, s = found
        code, rel = c.function_bytes(s["section"], s["value"])
        our_len = len(code)
        pat, mask = make_pattern(code, rel, min(a.len, our_len))

        lo = our_len * (1.0 - a.size_tol)
        hi = our_len * (1.0 + a.size_tol)
        ranked = []
        for b, e in funcs:
            ln = e - b
            if not (lo <= ln <= hi):
                continue
            fo = rva_to_file(secs, b)
            if fo is None:
                continue
            sc = score(exe[fo:fo + len(pat)], pat, mask)
            ranked.append((sc, b, ln))
        ranked.sort(reverse=True)
        print(f"\n{label}   (nosso: {our_len} bytes; candidatos por tamanho: {len(ranked)})")
        if not ranked:
            print("   nenhum candidato")
            continue
        for sc, b, ln in ranked[:a.top]:
            print(f"   {sc*100:6.1f}%  rva 0x{b:<9x} {ln:5d} bytes")
        if len(ranked) > 1:
            margin = (ranked[0][0] - ranked[1][0]) * 100
            verdict = "CONFIAVEL" if ranked[0][0] > 0.85 and margin > 15 else \
                      ("fraco" if ranked[0][0] > 0.7 else "INCONCLUSIVO")
            print(f"   margem sobre o 2o: {margin:.1f} pontos  -> {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
