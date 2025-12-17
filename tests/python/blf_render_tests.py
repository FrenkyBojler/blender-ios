#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
BLF font rendering test

Tests BLF text rendering using image comparison.
"""

import argparse
import os
import sys


def get_arguments(filepath, output_filepath):
    """Get command line arguments for rendering font test.
    
    Args:
        filepath: Path to the blend file (not used, we generate content directly)
        output_filepath: Where to save the rendered PNG
    """
    test_dir = os.path.dirname(__file__)
    return [
        "--background",
        "--factory-startup",
        "--enable-autoexec",
        "--debug-memory",
        "--debug-exit-on-error",
        "--python", os.path.join(test_dir, "font_render_script.py"),
        "--",
        output_filepath,
    ]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--testdir", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--blender", required=True)
    parser.add_argument("--oiiotool", required=True)
    parser.add_argument("--update", default=False, action="store_true")
    parser.add_argument("--batch", default=False, action="store_true")
    args = parser.parse_args()

    from modules import render_report
    
    report = render_report.Report("BLF Font Rendering", args.outdir, args.oiiotool)
    report.set_reference_dir("blf_renders")
    
    ok = report.run(args.testdir, args.blender, get_arguments, batch=args.batch)
    sys.exit(not ok)


if __name__ == "__main__":
    main()