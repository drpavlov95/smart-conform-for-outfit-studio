"""Render smooth, equal-camera comparisons of the exact live Blacksmith OBJs.

Run with Blender, not the system Python:
  blender --background --python tools/render_live_smooth_compare_blender.py -- \
    --output-dir tmp/diagnostics/live_smooth_compare
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys

import bpy
from mathutils import Vector


ROOT = Path(__file__).resolve().parent.parent
MESHES = (
    ("BAD", ROOT / "tmp/BlackSmithTorso_smart-conformed RM_Heavy slider.obj", 10.0),
    ("LATEST", ROOT / "tmp/latestblacksmithtorsopostsmartconform.obj", 10.0),
    ("CXX CHORD", ROOT / "tmp/cpp_posterior_chord_z6038.obj", 1.0),
    ("PAIRED HAND", ROOT / "tmp/BlackSmithTorso_hand-edit of the smart-conformed RM_Heavy slider.obj", 10.0),
    ("HISTORICAL", ROOT / "tmp/BlackSmithTorso_previously hand-sculped RM_Heavy slider.obj", 10.0),
)


def read_obj(path: Path, scale: float = 10.0):
    verts = []
    faces = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("v "):
                xyz = [float(value) * scale for value in line.split()[1:4]]
                verts.append(tuple(xyz))
            elif line.startswith("f "):
                face = []
                for token in line.split()[1:]:
                    face.append(int(token.split("/", 1)[0]) - 1)
                if len(face) >= 3:
                    faces.append(tuple(face))
    return verts, faces


def look_at(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


def add_label(text: str, x: float):
    curve = bpy.data.curves.new(f"label_{text}", "FONT")
    curve.body = text
    curve.align_x = "CENTER"
    curve.size = 4.0
    curve.extrude = 0.0
    obj = bpy.data.objects.new(f"label_{text}", curve)
    bpy.context.collection.objects.link(obj)
    obj.location = (x, -21.0, 92.0)
    obj.rotation_euler = (math.radians(-90.0), 0.0, 0.0)
    return obj


def main() -> int:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    output_dir = args.output_dir if args.output_dir.is_absolute() else ROOT / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    material = bpy.data.materials.new("neutral_cloth")
    material.use_nodes = True
    cloth_bsdf = material.node_tree.nodes.get("Principled BSDF")
    cloth_bsdf.inputs["Base Color"].default_value = (0.34, 0.48, 0.72, 1.0)
    cloth_bsdf.inputs["Roughness"].default_value = 0.72
    label_material = bpy.data.materials.new("labels")
    label_material.use_nodes = True
    label_bsdf = label_material.node_tree.nodes.get("Principled BSDF")
    label_bsdf.inputs["Base Color"].default_value = (0.01, 0.01, 0.01, 1.0)
    label_bsdf.inputs["Roughness"].default_value = 1.0

    offsets = (-128.0, -64.0, 0.0, 64.0, 128.0)
    for (label, path, scale), xoff in zip(MESHES, offsets):
        verts, faces = read_obj(path, scale)
        mesh = bpy.data.meshes.new(label)
        mesh.from_pydata(verts, [], faces)
        mesh.update()
        for polygon in mesh.polygons:
            polygon.use_smooth = True
        obj = bpy.data.objects.new(label, mesh)
        bpy.context.collection.objects.link(obj)
        obj.location.x = xoff
        obj.data.materials.append(material)
        text = add_label(label, xoff)
        text.data.materials.append(label_material)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    world = scene.world or bpy.data.worlds.new("comparison_world")
    scene.world = world
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (0.92, 0.92, 0.92, 1.0)
    background.inputs["Strength"].default_value = 0.65
    scene.render.resolution_x = 2048
    scene.render.resolution_y = 640
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False

    camera_data = bpy.data.cameras.new("comparison_camera")
    camera = bpy.data.objects.new("comparison_camera", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = 325.0
    camera_data.lens = 50.0

    for name, location, energy, size in (
        ("key", (-80.0, -120.0, 140.0), 2200.0, 95.0),
        ("fill", (130.0, -100.0, 80.0), 1400.0, 80.0),
        ("rim", (0.0, 80.0, 105.0), 1800.0, 75.0),
    ):
        light_data = bpy.data.lights.new(name, "AREA")
        light_data.energy = energy
        light_data.shape = "DISK"
        light_data.size = size
        light = bpy.data.objects.new(name, light_data)
        bpy.context.collection.objects.link(light)
        light.location = location
        look_at(light, (0.0, -5.0, 55.0))

    views = {
        "back": ((0.0, -210.0, 55.0), (0.0, -8.0, 55.0)),
        "backleft": ((115.0, -190.0, 62.0), (0.0, -7.0, 54.0)),
        "grazing_low": ((150.0, -175.0, 43.0), (0.0, -7.0, 49.0)),
    }
    for name, (location, target) in views.items():
        camera.location = location
        look_at(camera, target)
        scene.render.filepath = str(output_dir / f"{name}.png")
        bpy.ops.render.render(write_still=True)
        print(f"wrote {scene.render.filepath}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
