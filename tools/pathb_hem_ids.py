# Path B S0.1c: freeze the hem/tatter vertex-ID list from the AUTHORED base cloth.
#
# Why: the old hem metric selected "result verts with world z < 30", whose
# population CHANGES as gravity drops vertices - a candidate can game the
# metric by moving verts in/out of the band. This tool selects the hem from
# the AUTHORED base mesh once, by a documented geometric rule, and freezes the
# sorted ID list so every candidate is measured on the SAME material points.
#
# Rule (documented, not tuned): boundary vertices (vertices touching at least
# one edge that belongs to exactly one triangle) whose AUTHORED z lies in the
# lowest `frac` of the garment's own authored z-range. frac defaults to 0.20.
#
# Usage: pathb_hem_ids.py cloth_base.obj out_ids.txt [frac=0.20]
# Deterministic: same input -> byte-identical output (sorted IDs, fixed header).
import sys
import hashlib
import numpy as np


def load_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(idx) - 1):
                fs.append([idx[0], idx[k], idx[k + 1]])
    return np.array(vs), np.array(fs, dtype=int)


def main():
    base_path, out_path = sys.argv[1], sys.argv[2]
    frac = float(sys.argv[3]) if len(sys.argv) > 3 else 0.20
    verts, tris = load_obj(base_path)
    edge_count = {}
    for a, b, c in tris:
        for e in ((a, b), (b, c), (c, a)):
            key = (min(e), max(e))
            edge_count[key] = edge_count.get(key, 0) + 1
    boundary_verts = set()
    for (a, b), n in edge_count.items():
        if n == 1:
            boundary_verts.add(a)
            boundary_verts.add(b)
    zmin, zmax = verts[:, 2].min(), verts[:, 2].max()
    zcut = zmin + frac * (zmax - zmin)
    ids = sorted(v for v in boundary_verts if verts[v, 2] <= zcut)
    src_sha = hashlib.sha256(open(base_path, "rb").read()).hexdigest()[:12]
    with open(out_path, "w", newline="\n") as f:
        f.write(f"# pathb hem IDs | rule=boundary&authored-z<=zmin+{frac}*range\n")
        f.write(f"# source={base_path.replace(chr(92), '/')} sha256_12={src_sha}\n")
        f.write(f"# verts={len(verts)} boundary={len(boundary_verts)} "
                f"zrange=[{zmin:.3f},{zmax:.3f}] zcut={zcut:.3f} selected={len(ids)}\n")
        for i in ids:
            f.write(f"{i}\n")
    print(f"{out_path}: {len(ids)} hem IDs of {len(boundary_verts)} boundary "
          f"({len(verts)} verts), zcut={zcut:.2f}")


if __name__ == "__main__":
    main()
