#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This script runs clang-format on multiple files/directories.

While it can be called directly, you may prefer to run this from Blender's root directory with the command:

   make format

"""
__all__ = (
    "main",
)

import argparse
import multiprocessing
import os
import sys
import subprocess

from collections.abc import (
    Sequence,
)

VERSION_MIN = (20, 1, 8)
VERSION_MAX_RECOMMENDED = (20, 1, 8)
CLANG_FORMAT_CMD = "clang-format"

BASE_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(BASE_DIR)


extensions = (
    ".c", ".cc", ".cpp", ".cxx",
    ".h", ".hh", ".hpp", ".hxx",
    ".m", ".mm",
    ".osl", ".glsl", ".msl",
    ".metal",
)

extensions_only_retab = (
    ".cmake",
    "CMakeLists.txt",
    ".sh",
)

# Add files which are too large/heavy to format.
ignore_files: set[str] = set([
    # Currently empty, looks like.
    # "intern/cycles/render/sobol.cpp",
])

# Directories not to format (recursively).
#
# Notes:
# - These directories must also have a `.clang-format` that disables formatting,
#   so developers who use format-on-save functionality enabled don't have these files formatted on save.
# - The reason to exclude here is to prevent unnecessary work were the files would run through clang-format
#   only to do nothing because the `.clang-format` file prevents it.
ignore_directories = {
    "intern/itasc"
}


def compute_paths(paths: list[str], use_default_paths: bool) -> list[str]:
    # The resulting paths:
    # - Use forward slashes on all systems.
    # - Are relative to the GIT repository without any `.` or `./` prefix.

    # Optionally pass in files to operate on.
    if use_default_paths:
        paths = [
            "intern",
            "source",
            "tests/gtests",
        ]
    else:
        # Filter out files, this is only done so this utility wont print that it's
        # "Operating" on files that will be filtered out later on.
        paths = [
            f for f in paths
            if os.path.isdir(f) or (os.path.isfile(f) and f.endswith(extensions))
        ]

    if os.sep != "/":
        paths = [f.replace("/", os.sep) for f in paths]
    return paths


def source_files_from_git(paths: Sequence[str], changed_only: bool) -> list[str]:
    if changed_only:
        cmd = ("git", "diff", "HEAD", "--name-only", "-z", "--", *paths)
    else:
        cmd = ("git", "ls-tree", "-r", "HEAD", *paths, "--name-only", "-z")
    files = subprocess.check_output(cmd).split(b'\0')
    return [f.decode('utf-8') for f in files]


def convert_tabs_to_spaces(files: Sequence[str]) -> None:
    for f in files:
        print("TabExpand", f)
        with open(f, 'r', encoding="utf-8") as fh:
            data = fh.read()
            # Simple 4 space (but we're using 2 spaces).
            # `data = data.expandtabs(4)`

            # Complex 2 space
            # because some comments have tabs for alignment.
            def handle(line: str) -> str:
                line_strip = line.lstrip("\t")
                d = len(line) - len(line_strip)
                if d != 0:
                    return ("  " * d) + line_strip.expandtabs(4)
                return line.expandtabs(4)

            lines = data.splitlines(keepends=True)
            lines = [handle(line) for line in lines]
            data = "".join(lines)
        with open(f, 'w', encoding="utf-8") as fh:
            fh.write(data)


def clang_format_check_version(clang_format_cmd: str) -> tuple[int, int, int] | None:
    version_output = subprocess.check_output((clang_format_cmd, "-version")).decode('utf-8')

    # We print this after calling the command, so we don't print anything in case the file does not exist.
    print("Checking {}...".format(clang_format_cmd))

    version: str | None = next(iter(v for v in version_output.split() if v[0].isdigit()), None)
    if version is None:
        print("    Unable to detect 'clang-format -version'")
        return None

    version = version.split("-")[0]
    # Ensure exactly 3 numbers.
    version_num: tuple[int, int, int] = (tuple(int(n) for n in version.split(".")) + (0, 0, 0))[:3]  # type: ignore
    if version_num < VERSION_MIN:
        print("    Version of ", clang_format_cmd, " is too old:", version_num, "<", VERSION_MIN)
        return None

    return version_num


def clang_format_ensure_version() -> tuple[int, int, int] | None:
    global CLANG_FORMAT_CMD
    clang_format_cmd = None
    clang_format_version = None
    for i in range(2, -1, -1):
        clang_format_cmd = (
            "clang-format-" + (".".join(["{:d}"] * i).format(*VERSION_MIN[:i]))
            if i > 0 else
            "clang-format"
        )
        try:
            clang_format_version = clang_format_check_version(clang_format_cmd)
            if not clang_format_version:
                continue
        except FileNotFoundError:
            clang_format_version = None
            continue
        CLANG_FORMAT_CMD = clang_format_cmd
        break

    return clang_format_version


def clang_format_file(files: list[str]) -> bytes:
    cmd = [
        CLANG_FORMAT_CMD,
        # Update the files in-place.
        "-i",
        # Shows the list of processed files.
        "-verbose",
    ] + files
    return subprocess.check_output(cmd, stderr=subprocess.STDOUT)


def clang_print_output(output: bytes) -> None:
    print(output.decode('utf8', errors='ignore').strip())


def clang_format(files: list[str]) -> None:
    pool = multiprocessing.Pool()

    # Process in chunks to reduce overhead of starting processes.
    cpu_count = multiprocessing.cpu_count()
    chunk_size = min(max(len(files) // cpu_count // 2, 1), 32)
    for i in range(0, len(files), chunk_size):
        files_chunk = files[i:i + chunk_size]
        pool.apply_async(clang_format_file, args=[files_chunk], callback=clang_print_output)

    pool.close()
    pool.join()


def argparse_create() -> argparse.ArgumentParser:

    parser = argparse.ArgumentParser(
        description="Format C/C++/GLSL & Objective-C source code.",
        epilog=__doc__,
        # Don't re-wrap text, keep newlines & indentation.
        formatter_class=argparse.RawTextHelpFormatter,

    )
    parser.add_argument(
        "--expand-tabs",
        dest="expand_tabs",
        default=False,
        action='store_true',
        help="Run a pre-pass that expands tabs "
        "(default=False)",
        required=False,
    )
    parser.add_argument(
        "--changed-only",
        dest="changed_only",
        default=False,
        action='store_true',
        help=(
            "Format only edited files, including the staged ones. "
            "Using this with \"paths\" will pick the edited files lying on those paths. "
            "(default=False)"
        ),
        required=False,
    )
    parser.add_argument(
        "paths",
        nargs=argparse.REMAINDER,
        help="All trailing arguments are treated as paths.",
    )

    return parser


def main() -> int:
    version = clang_format_ensure_version()
    if not version:
        print("Could not find a suitable version of clang-format")
        return 1

    print("Using {:s} ({:d}.{:d}.{:d})...".format(CLANG_FORMAT_CMD, version[0], version[1], version[2]))

    args = argparse_create().parse_args()

    use_default_paths = not (bool(args.paths) or bool(args.changed_only))
    paths = compute_paths(args.paths, use_default_paths)
    # Check if user-defined paths exclude all clang-format sources.
    if args.paths and not paths:
        print("Skip clang-format: no target to format")
        return 0

    print("Operating on:" + (" ({:d} changed paths)".format(len(paths)) if args.changed_only else ""))
    for p in paths:
        print(" ", p)

    # Notes:
    # - Paths from GIT always use forward slashes (even on WIN32),
    #   so there is no need to convert slashes.
    # - Ensure a trailing slash so a `str.startswith` check can be used.
    ignore_directories_tuple = tuple(p.rstrip("/") + "/" for p in ignore_directories)

    files = [
        f for f in source_files_from_git(paths, args.changed_only)
        if f.endswith(extensions)
        if f not in ignore_files
        if not f.startswith(ignore_directories_tuple)

    ]

    if args.expand_tabs:
        # Always operate on all CMAKE files (when expanding tabs and no paths given).
        files_retab = [
            f for f in source_files_from_git((".",) if use_default_paths else paths, args.changed_only)
            if f.endswith(extensions_only_retab)
            if f not in ignore_files
            if not f.startswith(ignore_directories_tuple)
        ]
        convert_tabs_to_spaces(files + files_retab)

    clang_format(files)

    if version > VERSION_MAX_RECOMMENDED:
        print()
        print(
            "WARNING: Version of clang-format is too recent:",
            version, ">", VERSION_MAX_RECOMMENDED,
        )
        print(
            "You may want to install clang-format-{:d}.{:d}, "
            "or use the precompiled libs repository.".format(
                VERSION_MAX_RECOMMENDED[0], VERSION_MAX_RECOMMENDED[1],
            ),
        )
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
