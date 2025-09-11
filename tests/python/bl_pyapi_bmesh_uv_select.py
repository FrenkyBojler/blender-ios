# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_pyapi_bmesh_uv_select.py -- --verbose

__all__ = (
    "main",
)

# TODO: remove before committing to main.
'''
env UBSAN_OPTIONS=print_stacktrace=1 ASAN_OPTIONS=check_initialization_order=0:leak_check_at_exit=0 bash -c 'while true; do inotifywait -e close_write tests/python/bl_pyapi_bmesh_uv_select.py; tput clear && ./blender.bin --background --python tests/python/bl_pyapi_bmesh_uv_select.py -- --verbose; done'
'''


import bmesh
import unittest


def uv_select_sync_check_or_empty(bm):
    return bmesh.utils.uv_select_sync_check(bm) or {}


class TestBMeshUVSelectSimple(unittest.TestCase):

    def test_create_uvsphere(self):
        bm = bmesh.new()
        bmesh.ops.create_grid(
            bm,
            x_segments=3,
            y_segments=4,
            size=1.0,
        )

        self.assertEqual(len(bm.verts), 20)
        self.assertEqual(len(bm.edges), 31)
        self.assertEqual(len(bm.faces), 12)

        # Nothing selected.
        bm.uv_select_sync_valid = True
        self.assertEqual(uv_select_sync_check_or_empty(bm), {})

        # All verts selected, no UV's selected.
        for v in bm.verts:
            v.select = True

        bm.uv_select_sync_valid = True
        self.assertEqual(uv_select_sync_check_or_empty(bm).get(
            "count_uv_vert_none_selected_with_vert_selected", 0), 20)

        # No verts selected, all UV's selected.
        for v in bm.verts:
            v.select = False
        for f in bm.faces:
            for l in f.loops:
                l.uv_select_vert = True

        bm.uv_select_sync_valid = True
        self.assertTrue(uv_select_sync_check_or_empty(bm).get("count_uv_vert_any_selected_with_vert_unselected", 0), 48)

        bm.free()


def main():
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()


if __name__ == '__main__':
    main()
