#! /usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import sys
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(
        prog="blend_diff_git_driver",
        description="A custom git diff driver for .blend files.",
    )
    parser.add_argument("relative_path")
    parser.add_argument("old")
    parser.add_argument("old_hash")
    parser.add_argument("old_mode")
    parser.add_argument("new")
    parser.add_argument("new_hash")
    parser.add_argument("new_mode")
    args = parser.parse_args()
    run(args)


def run(args):
    p = args.relative_path
    if p.startswith("/"):
        p = p[1:]
    print(f"diff --git a/{p} b/{p}")
    print(f"--- a/{p}")
    print(f"+++ b/{p}")
    print("@@ -1 +100000 @@")
    sys.stdout.flush()
    blend_diff_path = Path(__file__).parent / "blend_diff"
    subprocess.run([blend_diff_path, args.old, args.new], check=True)


if __name__ == "__main__":
    main()
