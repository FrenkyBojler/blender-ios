#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Helper module / script to ensure that an empty directory exists at the provided path.

This is to be used for tests which may need to set BLENDER_USER_RESOURCES.

For specific tests running with CTests, this can be used with `set_tests_properties` and the
`resource_dir` fixture

Example:

    ./tests/ui_simulate/modules/resource_folder_ensure.py ./test_resource_folder
"""
__all__ = (
    "main",
    "ensure_directory"
)

import sys
import os


def ensure_directory(resource_path: str) -> bool:
    if os.path.exists(resource_path):
        if not os.path.isdir(resource_path) or len(os.listdir(resource_path)):
            print(f"{resource_path} is a file or non-empty directory. Unable to continue.", file=sys.stderr)
            return False
        print(f"{resource_path} exists and is empty.")
        return True

    try:
        os.mkdir(resource_path)
        print(f"Created empty directory at {resource_path}.")
        return True
    except Exception as e:
        print(f"Unable to create empty directory at {resource_path}. Unable to continue. Error: {e}", file=sys.stderr)
        return False


def main() -> int:
    return not ensure_directory(sys.argv[1])


if __name__ == "__main__":
    sys.exit(main())
