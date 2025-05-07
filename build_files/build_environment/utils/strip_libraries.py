#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Script which strips all libraries in the given library directory.

This will strip both static and shared libraries.

Usage:
  strip_libraries.py <path/to/library/directory>
"""
import argparse
import glob
import subprocess
import sys
import os
from pathlib import Path

def print_strip_lib(strip_lib, prev_print_len):
    print_str = f"Stripping: {strip_lib}"
    if prev_print_len > 0:
        print(f"\r{' '*prev_print_len}\r", end="")
    print(print_str, end="", flush=True)
    return len(print_str)

def strip_libs(strip_dir):
    print(f"Stripping libraries in: {strip_dir}")
    os.chdir(strip_dir)
    prev_print_len = 0;
    for shared_lib in glob.iglob("**/*.so*", recursive=True):
        shared_path = Path(shared_lib)
        if shared_path.suffix == ".py":
            # Work around badly named sycl scripts
            continue

        if shared_path.is_symlink():
            # Don't strip symlinks
            continue

        prev_print_len = print_strip_lib(shared_lib, prev_print_len)
        subprocess.check_call(["strip", "-s", "--enable-deterministic-archives", shared_lib])
    for static_lib in glob.iglob("**/*.a", recursive=True):
        static_path = Path(static_lib)

        if static_path.is_symlink():
            # Don't strip symlinks
            continue

        prev_print_len = print_strip_lib(static_lib, prev_print_len)
        subprocess.check_call(["objcopy", "--enable-deterministic-archives", static_lib])

    print("\nDone stripping libraries!")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument("directory", type=Path, help="Path to the library directory to strip")
    args = parser.parse_args()

    if sys.platform == "linux":
        strip_libs(args.bpy_dir)
        return


if __name__ == "__main__":
    main()
