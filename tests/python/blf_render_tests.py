#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import argparse

import os
import sys
from pathlib import Path


def get_test_script_dir():
    current_dir = os.path.dirname(os.path.realpath(__file__))
    test_scripts = os.path.join(current_dir, "bf_render_tests")
    return test_scripts


def get_arguments(filepath, output_filepath):
    """Get command line arguments for rendering font test.

    Args:
        output_filepath: Where to save the rendered PNG
    """
  
    # Windows separators get messed up when passing them inside the python expression
    output_filepath = output_filepath.replace("\\", "/")

    script_name = Path(filepath).stem + ".py"
    script_filepath = os.path.join(get_test_script_dir(), script_name)

    # build an expression which sets sys.argv and runs the file directly.
    # Allows for multiple tests in same Blender session via render_report.Report
    expr = (
        "import runpy, sys;"
        f"sys.argv=['{script_filepath}','--','{output_filepath}0001.png'];"
        "runpy.run_path(sys.argv[0], run_name='__main__')"
    )

    return [
        "--background",
        "--factory-startup",
        "--enable-autoexec",
        "--debug-memory",
        "--debug-exit-on-error",
        "--python-expr", expr,
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
    
    report = render_report.Report("BLF Font Rendering", args.outdir, args.oiiotool, extension=".py")
    report.set_reference_override_dir(Path(args.testdir) / "blf_renders") # Where test reference images are stored
    
    test_scripts = get_test_script_dir()
    ok = report.run(test_scripts, args.blender, get_arguments, batch=args.batch)
    sys.exit(not ok)


if __name__ == "__main__":
    main()