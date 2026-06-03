# Ghidra post-script: locate the conform call chain in OutfitStudio x64.exe.
#
# Anchors on the unique strings in OutfitStudioFrame::ConformShapes and dumps a
# 3-level call tree so we can identify:
#   ConformShapes -> ConformSliders -> OutfitProject::ConformShape
#       -> Automorph::BuildProximityCache, Automorph::GenerateResultDiff
# and separately tries to locate Automorph::ZeroVertDiff.
#
# Output: <project_dir>/drape_re_report.txt  (also printed to console)
#
# @category Drape

import os

fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()
refMgr = currentProgram.getReferenceManager()
memory = currentProgram.getMemory()

OUT = []
def emit(s):
    OUT.append(s)
    print(s)

def func_containing(addr):
    return fm.getFunctionContaining(addr)

def find_string_addrs(needle):
    """Return list of addresses of defined strings equal to / containing needle."""
    hits = []
    di = listing.getDefinedData(True)
    for d in di:
        try:
            t = d.getDataType().getName().lower()
        except:
            continue
        if "unicode" in t or "string" in t or "char" in t:
            v = d.getValue()
            if v is not None and needle in str(v):
                hits.append(d.getAddress())
    return hits

def funcs_referencing(addr):
    """Functions that reference the given (data) address."""
    out = set()
    for ref in refMgr.getReferencesTo(addr):
        f = func_containing(ref.getFromAddress())
        if f is not None:
            out.add(f)
    return out

def callees(func, depth, seen, prefix):
    if func is None or depth < 0:
        return
    if func.getEntryPoint() in seen:
        emit("%s%s @ %s  (revisited)" % (prefix, func.getName(), func.getEntryPoint()))
        return
    seen.add(func.getEntryPoint())
    emit("%s%s @ %s" % (prefix, func.getName(), func.getEntryPoint()))
    if depth == 0:
        return
    # iterate instructions, collect call targets in order
    it = listing.getInstructions(func.getBody(), True)
    seenCall = []
    while it.hasNext():
        ins = it.next()
        fl = ins.getFlowType()
        if fl is not None and fl.isCall():
            for r in ins.getReferencesFrom():
                tgt = fm.getFunctionAt(r.getToAddress())
                if tgt is not None and tgt.getEntryPoint() not in [c.getEntryPoint() for c in seenCall]:
                    seenCall.append(tgt)
    for c in seenCall:
        callees(c, depth - 1, seen, prefix + "    ")

emit("=== Drape RE report ===")
emit("program: %s" % currentProgram.getName())
emit("imagebase: %s" % currentProgram.getImageBase())

anchors = ["All shapes conformed.", "Conforming shapes...", "dlgConforming",
           "proximityRadiusSlider"]
conformShapesFuncs = set()
for a in anchors:
    addrs = find_string_addrs(a)
    emit("\n-- anchor '%s': %d string hit(s)" % (a, len(addrs)))
    for ad in addrs:
        fs = funcs_referencing(ad)
        for f in fs:
            emit("   ref by %s @ %s" % (f.getName(), f.getEntryPoint()))
            conformShapesFuncs.add(f)

emit("\n=== CALL TREE from anchored function(s) (depth 3) ===")
for f in conformShapesFuncs:
    emit("\n>>> root %s @ %s" % (f.getName(), f.getEntryPoint()))
    callees(f, 3, set(), "  ")

# Write report next to the project
try:
    proj_dir = os.environ.get("DRAPE_RE_OUT", None)
    if not proj_dir:
        proj_dir = "."
    path = os.path.join(proj_dir, "drape_re_report.txt")
    with open(path, "w") as fh:
        fh.write("\n".join(OUT))
    print("\nWROTE %s" % path)
except Exception as e:
    print("could not write report: %s" % e)
