#!/usr/bin/env python3
"""Locate OutfitStudio host functions in an official build, without symbols.

The hook needs the addresses of five OutfitStudio internals. They were found
once with Ghidra and hardcoded, which pins Drape to one exact BodySlide build -
every update turns Smart Conform off for every user with "hook was not
installed". This derives them instead:

  1. compile the SAME BodySlide source with symbols (each function lands in its
     own COMDAT section of the .obj, so its exact machine code is isolated),
  2. mask every byte the linker would patch (relocations) plus the immediates
     that legitimately differ between builds,
  3. search the official .text for what is left.

A pattern that matches exactly once is a located function. Zero or several
matches is reported as such and located NOTHING - a wrong address here is a
crash in the user's OutfitStudio, which is far worse than the honest refusal
the size guard already gives.
"""
from __future__ import annotations
import argparse
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------- COFF (.obj)

class Coff:
    def __init__(self, data: bytes):
        self.d = data
        # MSVC emits /bigobj for translation units with many COMDAT sections,
        # and its header and symbol records are wider than plain COFF. Every
        # OutfitStudio object built here is one, so this is the normal path.
        sig1, sig2 = struct.unpack_from("<HH", data, 0)
        self.big = (sig1 == 0 and sig2 == 0xFFFF)
        if self.big:
            self.nsec, self.symptr, self.nsym = struct.unpack_from("<III", data, 44)
            self.symsize = 20
            off = 56
            self.optsize = 0
        else:
            (self.machine, self.nsec, _ts, self.symptr, self.nsym,
             self.optsize, _ch) = struct.unpack_from("<HHIIIHH", data, 0)
            self.symsize = 18
            off = 20 + self.optsize
        self.sections = []
        for _ in range(self.nsec):
            name, vsize, vaddr, rawsize, rawptr, relptr, _lptr, nrel, _nl, ch = \
                struct.unpack_from("<8sIIIIIIHHI", data, off)
            self.sections.append(dict(name=name.rstrip(b"\0").decode("latin1"),
                                      rawsize=rawsize, rawptr=rawptr,
                                      relptr=relptr, nrel=nrel, ch=ch))
            off += 40
        strtab_off = self.symptr + self.nsym * self.symsize
        strtab_len = struct.unpack_from("<I", data, strtab_off)[0] if self.nsym else 4
        self.strtab = data[strtab_off:strtab_off + strtab_len]

    def _sym_name(self, raw: bytes) -> str:
        if raw[:4] == b"\0\0\0\0":
            off = struct.unpack_from("<I", raw, 4)[0]
            end = self.strtab.find(b"\0", off)
            return self.strtab[off:end].decode("latin1")
        return raw.rstrip(b"\0").decode("latin1")

    def find_symbol(self, needle: str):
        """First external symbol whose decorated name contains `needle`."""
        i = 0
        while i < self.nsym:
            off = self.symptr + i * self.symsize
            raw = self.d[off:off + 8]
            if self.big:
                value, secnum, _typ, sclass, naux = struct.unpack_from("<IiHBB", self.d, off + 8)
            else:
                value, secnum, _typ, sclass, naux = struct.unpack_from("<IhHBB", self.d, off + 8)
            name = self._sym_name(raw)
            if sclass == 2 and secnum > 0 and needle in name:      # IMAGE_SYM_CLASS_EXTERNAL
                return dict(name=name, section=secnum - 1, value=value)
            i += 1 + naux
        return None

    def function_bytes(self, secidx: int, value: int):
        s = self.sections[secidx]
        code = self.d[s["rawptr"]:s["rawptr"] + s["rawsize"]]
        rel = []
        for k in range(s["nrel"]):
            va, _sym, typ = struct.unpack_from("<IIH", self.d, s["relptr"] + k * 10)
            rel.append((va - value, typ))
        return code[value:], rel


# ------------------------------------------------------------------ PE (.exe)

def pe_text(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    imgbase = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    off = pe + 24 + optsize
    out = []
    for _ in range(nsec):
        name, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", data, off)
        nm = name.rstrip(b"\0").decode("latin1")
        if nm == ".text":
            out.append(dict(vaddr=vaddr, rawptr=rawptr,
                            size=min(rawsize, vsize) or rawsize))
        off += 40
    return out, imgbase


# ------------------------------------------------------------------- matching

# Relocation sites are linker-patched: their bytes cannot match across builds.
RELOC_WIDTH = {1: 8, 2: 4, 3: 4, 4: 4, 5: 4, 6: 8, 7: 4, 8: 4, 9: 4, 10: 4, 11: 4}


def make_pattern(code: bytes, rel, length: int):
    pat = bytearray(code[:length])
    mask = bytearray(b"\x01" * len(pat))
    for off, typ in rel:
        if off < 0 or off >= length:
            continue
        for k in range(off, min(off + RELOC_WIDTH.get(typ, 4), length)):
            mask[k] = 0
    # Direct CALL/JMP rel32 displacements differ between builds even without a
    # relocation record, so blank those four bytes too.
    i = 0
    while i < len(pat) - 4:
        if pat[i] == 0xE8 or pat[i] == 0xE9:
            for k in range(i + 1, min(i + 5, length)):
                mask[k] = 0
            i += 5
            continue
        i += 1
    return bytes(pat), bytes(mask)


def scan(hay: bytes, pat: bytes, mask: bytes, limit: int = 8):
    hits = []
    anchor = next((i for i, m in enumerate(mask) if m), None)
    if anchor is None:
        return hits
    a = pat[anchor]
    start = 0
    n = len(pat)
    while True:
        i = hay.find(bytes([a]), start)
        if i < 0 or len(hits) >= limit:
            break
        start = i + 1
        base = i - anchor
        if base < 0 or base + n > len(hay):
            continue
        seg = hay[base:base + n]
        if all(not mask[k] or seg[k] == pat[k] for k in range(n)):
            hits.append(base)
    return hits


TARGETS = [
    ("GenerateResultDiff", "?GenerateResultDiff@Automorph@@"),
    ("GetRawResultDiff", "?GetRawResultDiff@Automorph@@"),
    ("SetResultDiff", "?SetResultDiff@Automorph@@"),
    # GetDiffSet is a one-line forwarder in 5.8.2 - "return MaterializeSet(n)"
    # compiles to a 5-byte jmp whose only distinctive bytes are the masked
    # displacement, so it cannot be located and does not need to be: the
    # forwardee has the same signature and the same meaning.
    ("DiffSets::MaterializeSet",
     "?MaterializeSet@DiffDataSets@@AEAAPEAV?$unordered_map@GUVector3@nifly@@"
     "U?$hash@G@std@@U?$equal_to@G@4@V?$allocator@U?$pair@$$CBGUVector3@nifly@@"
     "@std@@@4@@std@@AEBV?$basic_string@D"),
    # DiffDataSets has two ZeroVertDiff overloads. The one the hook calls takes
    # (set, target, vector<uint16>*, unordered_map<uint16,float>*); the other
    # takes (set, int, float*). Pick by the vector parameter in the mangling.
    ("DiffSets::ZeroVertDiff",
     "?ZeroVertDiff@DiffDataSets@@QEAAXAEBV?$basic_string@DU?$char_traits@D@"
     "std@@V?$allocator@D@2@@std@@0PEAV?$vector@G"),
]


def pdata_starts(data: bytes) -> set:
    """Every function start the binary itself declares, from .pdata.

    This is the check that makes a byte match trustworthy. A pattern can in
    principle land in the middle of some unrelated function; a RUNTIME_FUNCTION
    entry beginning exactly at the located address cannot happen by accident,
    because the linker only emits one per real function.
    """
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack_from("<II", data, dd + 3 * 8)   # IMAGE_DIRECTORY_ENTRY_EXCEPTION
    if not rva or not size:
        return set()
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    off = pe + 24 + optsize
    secs = []
    for _ in range(nsec):
        _n, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", data, off)
        secs.append((vaddr, vsize or rawsize, rawptr))
        off += 40
    def to_file(r):
        for va, vs, rp in secs:
            if va <= r < va + vs:
                return rp + (r - va)
        return None
    fo = to_file(rva)
    if fo is None:
        return set()
    out = set()
    for k in range(size // 12):
        begin = struct.unpack_from("<I", data, fo + k * 12)[0]
        if begin:
            out.add(begin)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obj", action="append", required=True,
                    help="object file(s) from a symbol build of the same source")
    ap.add_argument("--exe", required=True, help="official OutfitStudio.exe")
    ap.add_argument("--len", type=int, default=64, help="pattern length in bytes")
    a = ap.parse_args()

    exe = Path(a.exe).read_bytes()
    texts, imgbase = pe_text(exe)
    if not texts:
        print("no .text section in", a.exe)
        return 1
    starts = pdata_starts(exe)
    print(f"exe {len(exe):,} bytes  imagebase 0x{imgbase:x}  "
          f".text rva 0x{texts[0]['vaddr']:x}  .pdata: {len(starts):,} funcoes")

    objs = [(p, Coff(Path(p).read_bytes())) for p in a.obj]
    print(f"{'funcao':>22s} {'bytes':>7s} {'mascarados':>11s} {'hits':>5s}   RVA")
    for label, needle in TARGETS:
        found = None
        for path, c in objs:
            s = c.find_symbol(needle)
            if s:
                found = (c, s)
                break
        if not found:
            print(f"{label:>22s} {'-':>7s} {'-':>11s} {'sem simbolo':>5s}")
            continue
        c, s = found
        code, rel = c.function_bytes(s["section"], s["value"])
        pat, mask = make_pattern(code, rel, min(a.len, len(code)))
        blanks = sum(1 for m in mask if not m)
        hits = []
        for t in texts:
            seg = exe[t["rawptr"]:t["rawptr"] + t["size"]]
            for h in scan(seg, pat, mask):
                hits.append(t["vaddr"] + h)
        if len(hits) == 1:
            ok = "INICIO DE FUNCAO" if hits[0] in starts else "!! meio de funcao"
            rva = f"0x{hits[0]:x}  {ok}"
        else:
            rva = "AMBIGUO" if len(hits) > 1 else "nao encontrado"
        print(f"{label:>22s} {len(pat):>7d} {blanks:>11d} {len(hits):>5d}   {rva}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
