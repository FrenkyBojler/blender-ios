#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Script which strips all libraries in the given library directory.
This is so we don't keep any debug data or symbols that contains
random hashes that are not reproducible between builds.

This will strip both static and shared libraries.

Usage:
  strip_libraries.py <path/to/library/directory>
"""
import argparse
import subprocess
import sys
from itertools import chain
from pathlib import Path

def get_strip_command(args) -> str:
    if sys.platform == "linux":
        return "strip"

    llvm_dir = str(args.llvm_bin)
    return llvm_dir + "/llvm-strip"

def get_objcopy_command(args) -> str:
    if sys.platform == "linux":
        return "objcopy"

    llvm_dir = str(args.llvm_bin)
    return llvm_dir + "/llvm-objcopy"


def strip_libs(args) -> None:
    strip_dir = args.directory
    print(f"Stripping libraries in: {strip_dir}")

    libs_to_strip = [strip_dir.rglob("*.so*")]
    if sys.platform == "darwin":
        libs_to_strip.append(strip_dir.rglob("*.dylib*"))

    for shared_lib in chain(*libs_to_strip):
        if shared_lib.suffix == ".py":
            # Work around badly named `sycl` scripts.
            continue

        if shared_lib.is_symlink():
            # Don't strip symbolic-links as we don't want to strip the same library multiple times.
            continue

        print(f"Stripping shared library: {shared_lib}")
        subprocess.check_call([get_strip_command(args), "-s", "--enable-deterministic-archives", shared_lib])
    for static_lib in strip_dir.rglob("*.a"):
        if static_lib.is_symlink():
            # Don't strip symbolic-links as we don't want to strip the same library multiple times.
            continue

        print(f"Stripping static library: {static_lib}")
        subprocess.check_call([get_objcopy_command(args), "--enable-deterministic-archives", static_lib])

    print("\nDone stripping libraries!")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument("directory", type=Path, help="Path to the library directory to strip")
    parser.add_argument("llvm_bin",
                        type=Path,
                        help="Path to the LLVM lib directory from which to obtain the strip/objcopy binaries on macOS")

    if sys.platform not in {"darwin", "linux"}:
        return

    strip_libs(parser.parse_args())


if __name__ == "__main__":
    main()
