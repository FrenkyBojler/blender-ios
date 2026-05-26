# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Regression test for the crash described in
# .context/blender_repro/HANDOFF_DEVELOPMENT_SPEC.md: re-activating a
# LayerCollection that lives under an excluded ancestor must not leave the
# depsgraph in an inconsistent state. The original crash was a parallel-
# evaluation race in Mesh::bounds_min_max() triggered by missing relations.

import unittest

from view_layer_common import (
    ViewLayerTesting,
    setup_extra_arguments,
)


def _find_layer_collection(layer_collection, target_collection):
    if layer_collection.collection == target_collection:
        return layer_collection
    for child in layer_collection.children:
        found = _find_layer_collection(child, target_collection)
        if found is not None:
            return found
    return None


class UnitTesting(ViewLayerTesting):
    def test_excluded_parent_re_activated_child(self):
        import bpy

        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for col in list(bpy.data.collections):
            bpy.data.collections.remove(col, do_unlink=True)
        for mesh in list(bpy.data.meshes):
            bpy.data.meshes.remove(mesh, do_unlink=True)

        scene = bpy.context.scene
        view_layer = bpy.context.view_layer

        parent_coll = bpy.data.collections.new("RegressionParent")
        child_coll = bpy.data.collections.new("RegressionChild")
        scene.collection.children.link(parent_coll)
        parent_coll.children.link(child_coll)

        parent_obj = bpy.data.objects.new('parent_obj',
                                          bpy.data.meshes.new('parent_mesh'))
        child_obj_a = bpy.data.objects.new('child_obj_a',
                                           bpy.data.meshes.new('child_mesh_a'))
        child_obj_b = bpy.data.objects.new('child_obj_b',
                                           bpy.data.meshes.new('child_mesh_b'))
        parent_coll.objects.link(parent_obj)
        child_coll.objects.link(child_obj_a)
        child_coll.objects.link(child_obj_b)
        view_layer.update()

        lc_parent = _find_layer_collection(view_layer.layer_collection,
                                           parent_coll)
        lc_child = _find_layer_collection(view_layer.layer_collection,
                                          child_coll)
        self.assertIsNotNone(lc_parent)
        self.assertIsNotNone(lc_child)

        lc_parent.exclude = True
        lc_child.exclude = True
        view_layer.update()

        lc_child.exclude = False
        view_layer.update()

        depsgraph = bpy.context.evaluated_depsgraph_get()

        # Force the bounds path that previously segfaulted via
        # GeometrySet::compute_boundbox_without_instances. Each evaluated_get
        # + to_mesh() asserts the specific child mesh was evaluable.
        child_eval_a = child_obj_a.evaluated_get(depsgraph)
        child_eval_b = child_obj_b.evaluated_get(depsgraph)
        mesh_a = child_eval_a.to_mesh()
        mesh_b = child_eval_b.to_mesh()
        self.assertIsNotNone(mesh_a)
        self.assertIsNotNone(mesh_b)
        child_eval_a.to_mesh_clear()
        child_eval_b.to_mesh_clear()

        evaluated_names = {ob.name for ob in depsgraph.objects}
        self.assertIn('child_obj_a', evaluated_names)
        self.assertIn('child_obj_b', evaluated_names)
        self.assertNotIn('parent_obj', evaluated_names)


if __name__ == '__main__':
    UnitTesting._extra_arguments = setup_extra_arguments(__file__)
    unittest.main()
