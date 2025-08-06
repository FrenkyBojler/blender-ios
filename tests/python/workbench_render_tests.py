#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2015-2022 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import platform
import sys
from pathlib import Path
from modules import render_report


def setup(bpy):
    for scene in bpy.data.scenes:
        scene.render.engine = 'BLENDER_WORKBENCH'
        scene.display.shading.light = 'STUDIO'
        scene.display.shading.color_type = 'TEXTURE'


class WorkbenchReport(render_report.Report):
    def __init__(self, title, output_dir, oiiotool, variation=None, blocklist=[]):
        super().__init__(title, output_dir, oiiotool, variation=variation, blocklist=blocklist)
        self.gpu_backend = variation

    def get_arguments(self, filepath, output_filepath):
        arguments = [
            "--background",
            "--factory-startup",
            "--enable-autoexec",
            "--debug-memory",
            "--debug-exit-on-error"]

        if self.gpu_backend:
            arguments.extend(["--gpu-backend", self.gpu_backend])

        arguments.extend([
            filepath,
            "-E", "BLENDER_WORKBENCH",
            "-P",
            os.path.realpath(__file__),
            "-o", output_filepath,
            "-F", "PNG",
            "-f", "1"])

        return arguments


def create_argparse():
    parser = argparse.ArgumentParser(
        description="Run test script for each blend file in TESTDIR, comparing the render result with known output."
    )
    parser.add_argument("--blender", required=True)
    parser.add_argument("--testdir", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--oiiotool", required=True)
    parser.add_argument('--batch', default=False, action='store_true')
    parser.add_argument('--gpu-backend')
    return parser


def main():
    parser = create_argparse()
    args = parser.parse_args()

    report = WorkbenchReport("Workbench", args.outdir, args.oiiotool, variation=args.gpu_backend)
    if args.gpu_backend == "vulkan":
        report.set_compare_engine('workbench', 'opengl')
    else:
        report.set_compare_engine('eevee', 'opengl')
    report.set_pixelated(True)
    report.set_reference_dir("workbench_renders")

    test_dir_name = Path(args.testdir).name
    if test_dir_name.startswith('hair') and platform.system() == "Darwin":
        report.set_fail_threshold(0.050)

    ok = report.run(args.testdir, args.blender, batch=args.batch)

    sys.exit(not ok)


if __name__ == "__main__":
    if not render_report.run_inside_blender(setup):
        main()
