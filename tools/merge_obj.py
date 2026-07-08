# Merge OBJ files into one (re-indexing faces). Usage: merge_obj.py out.obj in1.obj in2.obj ...
import sys

out = sys.argv[1]
with open(out, "w") as o:
    offset = 0
    for path in sys.argv[2:]:
        nv = 0
        for line in open(path):
            if line.startswith("v "):
                o.write(line)
                nv += 1
        offsets = offset
        for line in open(path):
            if line.startswith("f "):
                idx = [int(t.split("/")[0]) for t in line.split()[1:]]
                o.write("f " + " ".join(str(i + offsets) for i in idx) + "\n")
        offset += nv
print("wrote", out)
