# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# blender -b -P tests/python/bl_pyapi_bpy_utils.py -- --verbose

__all__ = (
    "main",
)

import os.path
import sys
import unittest

import bpy.utils


class UserResourceTest(unittest.TestCase):
    def test_user_resource_no_subdir(self) -> None:
        cache_dir = bpy.utils.user_resource('CACHES')

        match sys.platform:
            case 'darwin':
                expect = '/Library/Caches/Blender/'
            case 'win32':
                expect = '%USERPROFILE%\\AppData\\Local\\Blender Foundation\\Blender\\'
            case _:  # Linux or other POSIX-ish system.
                expect = '$HOME/.cache/blender/'
        expect = os.path.expandvars(expect)

        self.assertEqual(expect, cache_dir)

    def test_user_resource_with_subdir(self) -> None:
        cache_dir = bpy.utils.user_resource('CACHES', path="subdir")

        match sys.platform:
            case 'darwin':
                expect = '/Library/Caches/Blender/subdir/'
            case 'win32':
                expect = '%USERPROFILE%\\AppData\\Local\\Blender Foundation\\Blender\\subdir\\'
            case _:  # Linux or other POSIX-ish system.
                expect = '$HOME/.cache/blender/subdir/'
        expect = os.path.expandvars(expect)

        self.assertEqual(expect, cache_dir)


def main():
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()


if __name__ == '__main__':
    main()
