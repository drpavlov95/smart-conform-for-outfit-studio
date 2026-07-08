# Extract a slider diff from a BodySlide OSD file and apply it to a base OBJ.
# Usage: osd_extract.py file.osd DataName base.obj out.obj [scale=1.0]
import struct
import sys

def load_obj(path):
    vs, faces = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            faces.append(line)
    return vs, faces

def read_osd(path):
    data = open(path, "rb").read()
    off = 0
    magic, version, count = struct.unpack_from("<III", data, off)
    off += 12
    sets = {}
    for _ in range(count):
        (nlen,) = struct.unpack_from("<B", data, off)
        off += 1
        name = data[off:off + nlen].decode("ascii", "replace")
        off += nlen
        (dcount,) = struct.unpack_from("<H", data, off)
        off += 2
        diffs = []
        for _ in range(dcount):
            idx, x, y, z = struct.unpack_from("<Hfff", data, off)
            off += 14
            diffs.append((idx, x, y, z))
        sets[name] = diffs
    return sets

def main():
    osd, name, base, out = sys.argv[1:5]
    scale = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    sets = read_osd(osd)
    if name not in sets:
        print("available:", [k for k in sets if name.split("RM_")[0] in k][:20])
        sys.exit(1)
    vs, faces = load_obj(base)
    n = 0
    for idx, x, y, z in sets[name]:
        if idx < len(vs):
            vs[idx][0] += x * scale
            vs[idx][1] += y * scale
            vs[idx][2] += z * scale
            n += 1
    with open(out, "w") as o:
        for v in vs:
            o.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for f in faces:
            o.write(f)
    print(f"applied {n} diffs from '{name}' -> {out}")

if __name__ == "__main__":
    main()
