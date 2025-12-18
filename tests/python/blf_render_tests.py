#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import sys


def render_font_test():
    """Render the font test and save to output_path."""

    # Import BLF and image buffer utilities only when running inside Blender
    import blf
    import imbuf

    output_path = sys.argv[sys.argv.index("--") + 1]

    # Simple hardcoded configuration
    image_size = (512, 128)
    font_size = 24
    text = "The quick brown fox jumps over the lazy dog."
    text_position = (10, 90)

    # Create image buffer
    ibuf = imbuf.new(image_size)

    # Use default built-in font
    font_id = 0

    # Configure font
    blf.color(font_id, 1.0, 1.0, 1.0, 1.0)  # White text
    blf.size(font_id, font_size)
    blf.position(font_id, text_position[0], text_position[1], 0)
    blf.disable(font_id, blf.WORD_WRAP)

    with blf.bind_imbuf(font_id, ibuf, display_name="sRGB"):
        blf.draw_buffer(font_id, text)

    print(f"Saving image to: {output_path}")
    imbuf.write(ibuf, filepath=output_path)

    bpy.ops.wm.quit_blender()


# When run from inside Blender, render and exit.
try:
    import bpy
    inside_blender = True
except Exception:
    inside_blender = False


if inside_blender:
    render_font_test()
    sys.exit(0)

def get_arguments(filepath, output_filepath):
    """Get command line arguments for rendering font test.

    Args:
        output_filepath: Where to save the rendered PNG
    """

    return [
        "--background",
        "--factory-startup",
        "--enable-autoexec",
        "--debug-memory",
        "--debug-exit-on-error",
        "--python", os.path.realpath(__file__),
        "--",
        output_filepath + "0001.png",
    ]

def create_argparse():
    parser = argparse.ArgumentParser(
        description="Run test script for each blend file in TESTDIR, comparing the render result with known output."
    )
    parser.add_argument("--blender", required=True)
    parser.add_argument("--testdir", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--oiiotool", required=True)
    parser.add_argument("--update", default=False, action="store_true")
    parser.add_argument("--batch", default=False, action="store_true")
    return parser


def main():
    parser = create_argparse()
    args = parser.parse_args()

    from modules import render_report

    report = render_report.Report("BLF Font Rendering", args.outdir, args.oiiotool)
    report.set_reference_dir("blf_renders")

    ok = report.run(args.testdir, args.blender, get_arguments, batch=args.batch)
    sys.exit(not ok)


if __name__ == "__main__":
    main()
