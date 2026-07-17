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
        # Monotonic per-stroke generation for setStrokeGen (grab-class kernels
        # orig-stamp against it; must be nonzero — gen 0 collides with the
        # fresh orig-gen default and crashes).
        "stroke_gen",
        # Bound engine wrappers built lazily on the first stroke and reused:
        # the Mesh view, and the per-session Brush + CommandExecutor.
        "mesh_obj",
        "brush_obj",
        "executor",
        # Per-session undo history (wired to the executor as executor.meshLog);
        # each stroke is one step. Drives Tier-2 delta undo (see undo.py).
        "meshlog",
        # Mirror of the meshlog's applied-step count (curStep_): bumped on
        # stroke end, moved by undo/redo. Lets undo_decode seek to a target
        # step even across memfile-boundary transitions it isn't called for.
        "meshlog_cursor",
        # Reusable [main, SMOOTH] autosmooth program, rebuilt per stroke when
        # the brush's auto-smooth factor is nonzero.
        "program",
        # Dyntopo state for the current stroke (so stroke_end knows to call
        # endDynTopoStroke) and the reusable DynTopoParams.
        "dyntopo_active",
        "dtparams",
        "_freed",
    )

    def __init__(self, object_name, mesh_ptr, tree_ptr, verts_num):
        self.object_name = object_name
        self.mesh_ptr = mesh_ptr
        self.tree_ptr = tree_ptr
        self.verts_num = verts_num
        self.topo_stamp = engine.capi().lib.Mesh_topoStamp(mesh_ptr)
        self.generation = 0
        self.stroke_gen = 0
        self.mesh_obj = None
        self.brush_obj = None
        self.executor = None
        self.meshlog = None
        self.meshlog_cursor = 0
        self.program = None
        self.dyntopo_active = False
        self.dtparams = None
        self._freed = False

    def mesh(self):
        """Bound Mesh wrapper over the session's engine mesh (cached)."""
        if self.mesh_obj is None:
            mgr = engine.manager()
            self.mesh_obj = mgr.get_bound_pointer(
                mgr.get("sculptcore::mesh::Mesh"), self.mesh_ptr, deref=False)
        return self.mesh_obj

    def tree(self):
        """Bound SpatialTree wrapper (cached)."""
        mgr = engine.manager()
        return mgr.get_bound_pointer(
            mgr.get("sculptcore::spatial::SpatialTree"), self.tree_ptr, deref=False)

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
        # Owning engine wrappers (Brush, CommandExecutor, MeshLog) dispose their
        # C++ objects; the Mesh view is non-owning (freed via freeMesh below).
        # The executor goes before the meshlog it points at.
        for obj in (self.dtparams, self.program, self.executor, self.meshlog,
                    self.brush_obj):
            if obj is not None and not getattr(obj, "_disposed", False):
                obj.dispose()
        self.dtparams = None
        self.program = None
        self.executor = None
        self.meshlog = None
        self.brush_obj = None
        self.mesh_obj = None
        lib = engine.capi().lib
        if self.tree_ptr:
            lib.SpatialTree_free(self.tree_ptr)
            self.tree_ptr = None
        if self.mesh_ptr:
            lib.freeMesh(self.mesh_ptr)
            self.mesh_ptr = None
