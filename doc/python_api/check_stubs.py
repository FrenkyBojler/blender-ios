#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Type-check Blender Python examples and templates against generated stubs.
Each file is checked in a separate mypy process, running in parallel.

Usage:

   python doc/python_api/check_stubs.py

NOTE(@ideasman42): we are nowhere near close to having Blender scripts type check without any type warnings.
This is mainly as a way to check the stubs are valid, not as a way to ensure we have zero typing
errors - not yet at least.

"""

import argparse
import glob
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

# Project root derived from this file's location (doc/python_api/).
PROJECT_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))

STUB_DIR = os.path.join(PROJECT_ROOT, "doc", "python_api", "stubs")

SKIP = {
    "doc/python_api/examples/aud.py",
    "doc/python_api/examples/bpy.types.HydraRenderEngine.py",
    "scripts/templates_py/ui_list_generic.py",
}


def check_file(filepath: str) -> tuple[str, str]:
    """Run mypy on a single file, return (filepath, error_output)."""
    env = os.environ.copy()
    env["MYPYPATH"] = STUB_DIR
    result = subprocess.run(
        [
            sys.executable, "-m", "mypy", filepath,
            "--no-error-summary",
            "--explicit-package-bases",
        ],
        capture_output=True,
        text=True,
        env=env,
    )
    prefix = filepath + ":"
    lines = [l for l in result.stdout.splitlines()
             if l.startswith(prefix)
             # Mix-in classes that narrow `bl_*` Literal attributes cause diamond
             # inheritance conflicts - a `mypy` limitation, not a stub bug.
             and "incompatible with definition in base class" not in l]
    return filepath, "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-j", "--jobs", type=int, default=0,
        help="Parallel jobs (default 0 uses CPU count; 1 runs synchronously, "
             "streaming each mypy invocation's output directly to the terminal).",
    )
    args = parser.parse_args()

    os.chdir(PROJECT_ROOT)

    # Normalize backslashes for WIN32 so SKIP paths match.
    all_files = [
        f.replace("\\", "/") for f in (
            *glob.glob("doc/python_api/examples/*.py"),
            *glob.glob("scripts/templates_py/*.py"),
            *glob.glob("tests/python/*.py"),
            *glob.glob("scripts/modules/**/*.py", recursive=True),
            *glob.glob("scripts/startup/**/*.py", recursive=True),
            *glob.glob("scripts/addons_core/**/*.py", recursive=True),
        )
    ]
    files = sorted(f for f in all_files if f not in SKIP)

    errors = 0

    if args.jobs == 1:
        # Synchronous: print each file's result as soon as it's ready.
        for f in files:
            _, output = check_file(f)
            if output:
                errors += 1
                print(output)
                print()
    else:
        # Parallel mode. `max_workers=None` lets the pool pick the default.
        max_workers = args.jobs if args.jobs > 0 else None
        results: dict[str, str] = {}
        with ProcessPoolExecutor(max_workers=max_workers) as pool:
            futures = {pool.submit(check_file, f): f for f in files}
            for future in as_completed(futures):
                filepath, output = future.result()
                results[filepath] = output

        # Print results in file order.
        for f in files:
            output = results[f]
            if output:
                errors += 1
                print(output)
                print()

    print("Checked {:d} files, {:d} with errors.".format(len(files), errors))


if __name__ == "__main__":
    main()
