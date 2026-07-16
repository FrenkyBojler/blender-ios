# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Per-object mode session: the engine-side objects (mesh, spatial tree) plus
the bookkeeping the conversion layer needs (topology stamp for the fast
path, generation counter bumped on every rebuild so stale handles are
detectable).
"""

from . import engine


class Session:
    __slots__ = (
        "object_name",
        "mesh_ptr",
        "tree_ptr",
        "verts_num",
        "topo_stamp",
        "generation",
        "_freed",
    )

    def __init__(self, object_name, mesh_ptr, tree_ptr, verts_num):
        self.object_name = object_name
        self.mesh_ptr = mesh_ptr
        self.tree_ptr = tree_ptr
        self.verts_num = verts_num
        self.topo_stamp = engine.capi().lib.Mesh_topoStamp(mesh_ptr)
        self.generation = 0
        self._freed = False

    def topology_changed(self):
        """True when a topology op ran since import — original Blender
        indices are then no longer valid (slow-path export required)."""
        return engine.capi().lib.Mesh_topoStamp(self.mesh_ptr) != self.topo_stamp

    def free(self):
        # Re-entrant: exit() may run twice (forced exit at unregister plus
        # the addon's own teardown).
        if self._freed:
            return
        self._freed = True
        lib = engine.capi().lib
        if self.tree_ptr:
            lib.SpatialTree_free(self.tree_ptr)
            self.tree_ptr = None
        if self.mesh_ptr:
            lib.freeMesh(self.mesh_ptr)
            self.mesh_ptr = None
