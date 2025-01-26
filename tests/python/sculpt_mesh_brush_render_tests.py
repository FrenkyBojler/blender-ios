#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import sys


def set_view3d_context_override(context_override):
    """
    Set context override to become the first viewport in the active workspace

    The ``context_override`` is expected to be a copy of an actual current context
    obtained by `context.copy()`
    """

    for area in context_override["screen"].areas:
        if area.type != 'VIEW_3D':
            continue
        for space in area.spaces:
            if space.type != 'VIEW_3D':
                continue
            for region in area.regions:
                if region.type != 'WINDOW':
                    continue
                context_override["area"] = area
                context_override["region"] = region


def prepare_sculpt_scene(context: any):
    """
    Prepare a clean state of the scene suitable for benchmarking

    It creates a high-res object and moves it to a sculpt mode.
    """

    import bpy

    # Ensure the current mode is object, as it might not be the always the case
    # if the benchmark script is run from a non-clean state of the .blend file.
    if context.object:
        bpy.ops.object.mode_set(mode='OBJECT')

    # Delete all current objects from the scene.
    # bpy.ops.object.select_all(action='SELECT')
    # bpy.ops.object.delete(use_global=False)
    # bpy.ops.outliner.orphans_purge()

    bpy.ops.mesh.primitive_monkey_add(size=2, align='WORLD', location=(0, 0, 0), scale=(1, 1, 1,))

    bpy.ops.object.subdivision_set(level=5)
    bpy.ops.object.modifier_apply(modifier="Subdivision")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.mode_set(mode='SCULPT')


def generate_stroke(context):
    """
    Generate stroke for the bpy.ops.sculpt.brush_stroke operator

    The generated stroke covers the full plane diagonal.
    """
    from mathutils import Vector

    template = {
        "name": "stroke",
        "mouse": (0.0, 0.0),
        "mouse_event": (0, 0),
        "location": (0.0, 0.0, 0.0),
        "is_start": True,
        "pressure": 1.0,
        "time": 1.0,
        "size": 1.0,
        "x_tilt": 0,
        "y_tilt": 0
    }

    num_steps = 250
    start = Vector((context['area'].width, context['area'].height))
    end = Vector((0, 0))
    delta = (end - start) / (num_steps - 1)

    stroke = []
    for i in range(num_steps):
        step = template.copy()
        step["mouse"] = start + delta * i
        step["mouse_event"] = start + delta * i
        stroke.append(step)

    return stroke


def setup():
    """
    Prepare the scene for rendering - generates objects then performs a stroke
    """

    import bpy
    context = bpy.context

    # Create an undo stack explicitly. This isn't created by default in background mode.
    bpy.ops.ed.undo_push()

    prepare_sculpt_scene(context)

    context_override = context.copy()
    set_view3d_context_override(context_override)

    with context.temp_override(**context_override):
        bpy.ops.sculpt.brush_stroke(stroke=generate_stroke(context_override), override_location=True)


try:
    import bpy
    inside_blender = True
except ImportError:
    inside_blender = False


if inside_blender:
    try:
        setup()
    except Exception as e:
        print(e)
        sys.exit(1)


def get_arguments(filepath, output_filepath):
    dirname = os.path.dirname(filepath)

    args = [
        "--background",
        "--factory-startup",
        "--enable-autoexec",
        "--debug-memory",
        "--debug-exit-on-error",
        "-E", "BLENDER_WORKBENCH",
        filepath,
        "-P", os.path.realpath(__file__),
        "-o", output_filepath,
        "-f", "1",
        "-x", "1",
        "-F", "PNG"]

    return args


def create_argparse():
    parser = argparse.ArgumentParser(
        description="Run test script for each blend file in TESTDIR, comparing the render result with known output."
    )
    parser.add_argument("--blender", required=True)
    parser.add_argument("--testdir", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--oiiotool", required=True)
    parser.add_argument("--batch", default=False, action="store_true")
    return parser


def main():
    parser = create_argparse()
    args = parser.parse_args()

    from modules import render_report
    report = render_report.Report("Sculpt - Mesh Brushes", args.outdir, args.oiiotool)
    report.set_pixelated(True)
    # Default error tolerances are quite large, lower them.
    report.set_fail_threshold(2.0 / 255.0)
    report.set_fail_percent(0.01)
    report.set_reference_dir("reference")

    ok = report.run(args.testdir, args.blender, get_arguments, batch=args.batch)

    sys.exit(not ok)


if not inside_blender and __name__ == "__main__":
    main()
