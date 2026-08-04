#!/usr/bin/env python3
"""Disassemble a located host function and resolve everything it calls.

Byte-scanning for E8 finds calls that are not there (an E8 inside another
instruction's operand) and misses the ones that are (calls routed through
incremental-link thunks). Both happened here. This walks real instructions.

Thunks are followed: MSVC incremental linking turns "call f" into "call thunk"
where the thunk is a lone "jmp f", and the thunk itself is not a declared
function, so a naive .pdata filter throws away exactly the edges we want.
"""
from __future__ import annotations
import argparse
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64          # noqa: E402
sys.path.insert(0, str(Path(__file__).parent))
from rank_host_funcs import pdata_functions, rva_to_file   # noqa: E402


def read(exe, secs, rva, n):
    fo = rva_to_file(secs, rva)
    return exe[fo:fo + n] if fo is not None else b""


def resolve_thunk(exe, secs, rva, starts, depth=3):
    """Follow lone jmp thunks to the function they stand for."""
    for _ in range(depth):
        if rva in starts:
            return rva, False
        b = read(exe, secs, rva, 6)
        if len(b) >= 5 and b[0] == 0xE9:
            rva = rva + 5 + struct.unpack_from("<i", b, 1)[0]
            continue
        if len(b) >= 6 and b[0] == 0xFF and b[1] == 0x25:      # jmp [rip+d]
            return rva, True
        break
    return rva, rva not in starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--rva", required=True, help="hex rva of the function to trace")
    ap.add_argument("--mark", action="append", default=[],
                    help="hex rva(s) to flag if they show up as callees")
    a = ap.parse_args()

    exe = Path(a.exe).read_bytes()
    funcs, secs = pdata_functions(exe)
    starts = {b for b, _e in funcs}
    extent = {b: e for b, e in funcs}
    rva = int(a.rva, 16)
    if rva not in extent:
        print(f"0x{rva:x} nao e' inicio de funcao declarado")
        return 1
    end = extent[rva]
    code = read(exe, secs, rva, end - rva)
    marks = {int(m, 16) for m in a.mark}

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    seq, seen = [], {}
    for ins in md.disasm(code, rva):
        if ins.mnemonic != "call":
            continue
        op = ins.op_str.strip()
        if op.startswith("0x"):
            tgt = int(op, 16)
            real, indirect = resolve_thunk(exe, secs, tgt, starts)
            kind = "thunk->" if real != tgt else ""
            seq.append((ins.address - rva, real, kind, indirect))
        else:
            seq.append((ins.address - rva, None, f"indireto {op}", False))

    print(f"funcao 0x{rva:x}..0x{end:x} ({end - rva} bytes)  chamadas: {len(seq)}")
    order = []
    for off, tgt, kind, ind in seq:
        if tgt is None:
            continue
        if tgt not in seen:
            seen[tgt] = len(order)
            order.append(tgt)
    print(f"alvos diretos distintos: {len(order)}\n")
    for off, tgt, kind, ind in seq:
        if tgt is None:
            print(f"  +0x{off:04x}  {kind}")
            continue
        flag = "   <=== MARCADO" if tgt in marks else ""
        real = "inicio-de-funcao" if tgt in starts else "?? nao declarado"
        sz = extent.get(tgt, 0) - tgt if tgt in extent else 0
        print(f"  +0x{off:04x}  {kind}0x{tgt:<9x} {sz:5d}B  {real}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
