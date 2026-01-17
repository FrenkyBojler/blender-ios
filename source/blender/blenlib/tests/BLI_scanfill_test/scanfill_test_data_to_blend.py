#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024 Campbell Barton
#
# SPDX-License-Identifier: GPL-2.0-or-later

__all__ = (
    "main",
)

import json
import sys
import os

BASE_DIR = os.path.abspath(os.path.dirname(__file__))


def load_test_data_as_mesh(filepath: str) -> None:
    import bmesh  # type: ignore
    import bpy  # type: ignore

    name_only = os.path.basename(filepath).removesuffix(".json")

    with open(filepath, "r") as fh:
        data = json.load(fh)

    me = bpy.data.meshes.new(name=name_only)
    bm = bmesh.new()

    for v in data["verts"]:
        bm.verts.new((v[0], v[1], 0.0))

    verts = list(bm.verts)
    for e in data["edges"]:
        bm.edges.new((verts[e[0]], verts[e[1]]))

    bm.to_mesh(me)
    bm.free()

    ob = bpy.data.objects.new(name=name_only, object_data=me)
    bpy.context.collection.objects.link(ob)


def main() -> int:
    import bpy

    bpy.ops.wm.read_factory_settings(use_empty=True)
    data_dir = os.path.join(BASE_DIR, "data")
    files = [f for f in os.listdir(data_dir) if f.endswith(".json")]
    files.sort()

    for f in files:
        load_test_data_as_mesh(os.path.join(data_dir, f))

    blend_filepath = os.path.join(BASE_DIR, "data_source.blend")
    bpy.ops.wm.save_mainfile(
        'EXEC_DEFAULT',
        filepath=blend_filepath,
    )
    print("Written:", blend_filepath)
    return 0


if __name__ == "__main__":
    sys.exit(main())
