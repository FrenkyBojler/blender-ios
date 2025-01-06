# SPDX-FileCopyrightText: 2019-2023 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# XXX based on tests/python/bl_id_management.py, only designed for develpment tests.

import bpy
import unittest
import random


class TestRNAIDTypes(unittest.TestCase):
    data_container_id = 'meshes'
    default_name = "Mesh"

    def test_rna_idtypes(self):
        # This test the coherence between ID types exposed in `bpy.data`, and these listed in `rna_enum_id_type_items`.
        new_args_extra = {
            "curves": {'type': 'CURVE'},
            "lightprobes": {'type': 'SPHERE'},
            "node_groups": {'type': 'CompositorNodeTree'},
            "textures": {'type': 'NONE'},
        }
        for container_id in dir(bpy.data):
            if container_id.startswith(("rna", "__", "version")):
                continue
            container = getattr(bpy.data, container_id)
            if callable(container):
                continue
            if not hasattr(container, "__len__"):
                continue
            if len(container) == 0:
                if not hasattr(container, "new"):
                    continue
                if container_id in new_args_extra:
                    container.new("TestData", **new_args_extra[container_id])
                else:
                    container.new("TestData")
            self.assertIsNot(container[0].id_type, "")


class TestHelper:

    @property
    def data_container(self):
        return getattr(bpy.data, self.data_container_id)

    def clear_container(self):
        bpy.data.batch_remove(self.data_container)

    def add_to_container(self, name=""):
        return self.data_container.new(name)

    def remove_from_container(self, data=None, name=None, index=None):
        data_container = self.data_container
        if not data:
            if name:
                data = data_container[name]
            elif index:
                data = data_container[index]
        self.assertTrue(data is not None)
        data_container.remove(data)

    def add_items_with_randomized_names(self, number_items=1, name_prefix=None, name_suffix=""):
        if name_prefix is None:
            name_prefix = self.default_name
        for i in range(number_items):
            self.add_to_container(name=name_prefix + str(random.random())[2:5] + name_suffix)

    def ensure_proper_order(self):
        id_prev = None
        for id in self.data_container:
            if id_prev:
                self.assertTrue(id_prev.name < id.name or id.library)
            id_prev = id


# XXX Test only, used to quickly do some raw performance testings of new PointerRNA code.
# To be removed before committing in main.
class TestBpyRnaPerformances(TestHelper, unittest.TestCase):
    data_container_id = 'objects'
    default_name = "Object"

    def test_perfs(self):
        self.clear_container()
        for i in range(100):
            ob = self.data_container.new(name="", object_data=None)
            ob.asset_mark()

        for i in range(100000):
            for ob in self.data_container:
                ob_asset = ob.asset_data
                del ob_asset


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
