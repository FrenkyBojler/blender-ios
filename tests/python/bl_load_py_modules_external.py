# SPDX-FileCopyrightText: 2011-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Load non-Blender Python modules.
#
# This is to ensure that Blender-bundled modules can actually be loaded at runtime.

"""
blender -b --factory-startup -P tests/python/bl_load_py_modules_external.py
"""

import sys
import traceback
import unittest
import importlib

# These are the direct dependencies of Blender. Indirect ones aren't tested here, but if we ever run into a situation
# where this approach is too limiting, just add more modules here.
MODULES_TO_LOAD = (
    'autopep8',
    'cattrs',
    'fastjsonschema',
    'numpy',
    'requests',
    'typing_extensions',
    'zstandard',
)


class ModuleLoadTest(unittest.TestCase):
    def test_load_modules(self) -> None:
        failed_modules: list[str] = []

        for module_name in MODULES_TO_LOAD:
            try:
                importlib.import_module(module_name)
            except ImportError:
                traceback.print_exc()
                failed_modules.append(module_name)

        if failed_modules:
            self.fail("modules failed to load: {}".format(failed_modules))


def main() -> None:
    if "--" in sys.argv:
        argv = sys.argv[sys.argv.index("--") + 1:].copy()
    else:
        argv = []

    unittest.main(
        argv=[sys.argv[0], *argv],
    )


if __name__ == "__main__":
    main()
