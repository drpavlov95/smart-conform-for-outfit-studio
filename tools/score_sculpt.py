# Build hand-sculpt references for every corpus garment (base.obj + OSD RM_Heavy
# diff) and score shape-match OFF vs ON against them.
import struct, sys, os
import numpy as np

SD = r"E:\Wabbajack\InstallPacks\mods\Rley Mods - Vanilla Refits\CalienteTools\BodySlide\ShapeData"
VAL = r"E:\SkyrimDev\Drape\tmp\smval"

OSD = {
    "falmer_rm_heavy":         SD + r"\CBBE 3BA Vanilla (Im Physics)\Body\Falmer.osd",
    "yarl_rm_heavy":           SD + r"\CBBE 3BA Vanilla (No Physics)\Body\Yarl Clothes 2.osd",
    "fur_blacksmith_rm_heavy": None,   # split: Fur 1 + Blacksmith
    "vampire_rm_heavy":        SD + r"\CBBE 3BA Dawnguard (Im Physics)\Body\Vampire Armor.osd",
}
FUR = SD + r"\CBBE 3BA Vanilla (Physics)\Body\Fur 1.osd"
BLACKSMITH = SD + r"\CBBE 3BA Vanilla (Physics)\Body\Blacksmith.osd"

def read_osd(path):
    data = open(path, "rb").read()
    off = 0
    magic, version, count = struct.unpack_from("<III", data, off); off += 12
    sets = {}
    for _ in range(count):
        (nlen,) = struct.unpack_from("<B", data, off); off += 1
        name = data[off:off+nlen].decode("ascii", "replace"); off += nlen
        (dcount,) = struct.unpack_from("<H", data, off); off += 2
        diffs = []
        for _ in range(dcount):
            idx, x, y, z = struct.unpack_from("<Hfff", data, off); off += 14
            diffs.append((idx, x, y, z))
        sets[name] = diffs
    return sets

def load(p):
    return np.array([[float(x) for x in l.split()[1:4]] for l in open(p) if l.startswith("v ")])

osd_cache = {}
def sets_for(path):
    if path not in osd_cache: osd_cache[path] = read_osd(path)
    return osd_cache[path]

rows = []
for f in sorted(os.listdir(VAL)):
    if not f.endswith("_base.obj"): continue
    tag = f[:-9]                       # corp_shape
    corp = next(c for c in OSD if tag.startswith(c))
    shape = tag[len(corp)+1:]          # underscored shape name
    if corp == "fur_blacksmith_rm_heavy":
        paths = [BLACKSMITH, FUR]
    else:
        paths = [OSD[corp]]
    cands = []
    for p in paths:
        for name in sets_for(p):
            if not name.endswith("RM_Heavy"): continue
            base_name = name[:-8].replace(" ", "_").rstrip("_")
            if base_name.lower() == shape.lower():
                cands.append((p, name))
    if not cands:
        # fuzzy: substring either way
        for p in paths:
            for name in sets_for(p):
                if name.endswith("RM_Heavy") and (shape.lower().replace("_","") in name.lower().replace(" ","").replace("_","")):
                    cands.append((p, name))
    if not cands:
        rows.append((tag, None, None, None, "NO OSD MATCH"))
        continue
    p, name = cands[0]
    base = load(os.path.join(VAL, f))
    diffs = sets_for(p)[name]
    ref = base.copy()
    napp = 0
    for idx, x, y, z in diffs:
        if idx < len(ref):
            ref[idx] += (x, y, z); napp += 1
    try:
        off_v = load(os.path.join(VAL, tag + "_off.obj"))
        on_v  = load(os.path.join(VAL, tag + "_on.obj"))
    except FileNotFoundError:
        rows.append((tag, name, None, None, "missing result obj"))
        continue
    if len(off_v) != len(ref) or len(on_v) != len(ref):
        rows.append((tag, name, None, None, f"vert mismatch {len(ref)}/{len(off_v)}"))
        continue
    eo = np.linalg.norm(off_v - ref, axis=1)
    en = np.linalg.norm(on_v - ref, axis=1)
    rows.append((tag, name, eo, en, None))

print(f"{'garment':42s} {'diffN':>5} {'OFF mean/p95/max':>20} {'ON mean/p95/max':>20}  verdict")
for tag, name, eo, en, err in rows:
    if err:
        print(f"{tag:42s} {err}")
        continue
    dm = en.mean() - eo.mean()
    dp = np.percentile(en,95) - np.percentile(eo,95)
    verdict = "BETTER" if (dm < -0.01 or (abs(dm) <= 0.01 and dp < -0.01)) else ("WORSE" if (dm > 0.01) else "same")
    print(f"{tag:42s} {'':5s} {eo.mean():6.3f}/{np.percentile(eo,95):5.2f}/{eo.max():5.2f} "
          f"{en.mean():6.3f}/{np.percentile(en,95):5.2f}/{en.max():5.2f}  {verdict}")
