#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
`make benchmark` helper

* Creates relevant directory if none exists and populates with BUILD_DIR binary
* Runs the default profile
"""

__all__ = {
    "main"
}

import argparse
import os
import sys

from make_utils import call


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_directory")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()

    benchmark_dir = os.path.join(os.path.pardir, "benchmark")
    if not os.path.exists(benchmark_dir):
        build_dir = args.build_directory
        if sys.platform == "darwin":
            blender_bin = os.path.join(build_dir, "bin/Blender.app/Contents/MacOS/Blender")
        else:
            blender_bin = os.path.join(build_dir, os.path.normcase("bin/blender"))

        if not os.path.isfile(blender_bin):
            sys.stderr.write("blender binary not found, can't initialize benchmarks")
            return 1

        create_dir_command = ["./tests/performance/benchmark.py", "init", "--blender_bin", os.path.abspath(blender_bin)]
        exitcode = call(create_dir_command)
        if exitcode != 0:
            return exitcode

    run_command = ["./tests/performance/benchmark.py", "run", "default"]
    return call(run_command)


if __name__ == "__main__":
    sys.exit(main())
