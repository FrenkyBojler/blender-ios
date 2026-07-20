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
        # Blender-side vertex count at the last enter/flush. The engine may
        # run ahead of the Mesh (write-back is deferred to the mode flush), so
        # foreign-change detection compares the Mesh against this, never
        # against the live engine count.
        "blender_verts_num",
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
        # The object's ID.session_uid, the key the external draw provider is
        # registered under (P5 D6). 0 when external draw is unavailable.
        "draw_key",
        # Reusable [main, SMOOTH] autosmooth program, rebuilt per stroke when
        # the brush's auto-smooth factor is nonzero.
        "program",
        # Dyntopo state for the current stroke (so stroke_end knows to call
        # endDynTopoStroke) and the reusable DynTopoParams.
        "dyntopo_active",
        "dtparams",
        # Multires sessions (P8): the engine Multires stack + the cage mesh it
        # was built over (both owned; mesh_ptr/tree_ptr are then non-owning
        # views of the stack's active level), the cached sample<->vertex map,
        # the stack's top level, the currently active (sculpt) level, and the
        # modifier's show_viewport state to restore on exit. multires_ptr is
        # None for plain-Mesh sessions.
        "multires_ptr",
        "cage_ptr",
        "multires_map",
        "multires_level",
        "multires_active_level",
        "multires_show_viewport",
        # Store snapshot after the latest undo push (bytes) — the next push's
        # pre-state, and the C4 blob-fallback base for level-crossing undo.
        "multires_last_blob",
        # User attribute layers (UV maps, colors, custom attrs) seeded into the
        # engine on enter and recreated on the Blender mesh after a topology
        # rebuild (which drops all customdata). A list of descriptor dicts; see
        # convert._load_bridged_attrs. Empty for multires sessions (deferred).
        "bridged_attrs",
        # Name of the active POINT/FLOAT_COLOR color attribute at enter, so the
        # color write-back recreates it under its own name after a rebuild.
        "color_attr_name",
        # Engine UVs diverged from the Blender mesh (UV-project op, UV
        # reprojection): every flush writes the engine `uv` column back into
        # the active UV map. Sticky for the session — undo decodes re-flush.
        "uv_dirty",
        "_freed",
    )

    def __init__(self, object_name, mesh_ptr, tree_ptr, verts_num):
        self.object_name = object_name
        self.mesh_ptr = mesh_ptr
        self.tree_ptr = tree_ptr
        self.verts_num = verts_num
        self.blender_verts_num = verts_num
        self.topo_stamp = engine.capi().lib.Mesh_topoStamp(mesh_ptr)
        self.generation = 0
        self.stroke_gen = 0
        self.mesh_obj = None
        self.brush_obj = None
        self.executor = None
        self.meshlog = None
        self.meshlog_cursor = 0
        self.draw_key = 0
        self.program = None
        self.dyntopo_active = False
        self.dtparams = None
        self.multires_ptr = None
        self.cage_ptr = None
        self.multires_map = None
        self.multires_level = 0
        self.multires_active_level = 0
        self.multires_show_viewport = True
        self.multires_last_blob = None
        self.bridged_attrs = []
        self.color_attr_name = None
        self.uv_dirty = False
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
        if self.multires_ptr:
            # mesh_ptr/tree_ptr are the stack's active-level views (stack-owned);
            # the cage outlives the stack (Multires_new does not own it).
            self.tree_ptr = None
            self.mesh_ptr = None
            lib.Multires_free(self.multires_ptr)
            self.multires_ptr = None
            if self.cage_ptr:
                lib.freeMesh(self.cage_ptr)
                self.cage_ptr = None
            self.multires_map = None
            return
        if self.tree_ptr:
            lib.SpatialTree_free(self.tree_ptr)
            self.tree_ptr = None
        if self.mesh_ptr:
            lib.freeMesh(self.mesh_ptr)
            self.mesh_ptr = None
