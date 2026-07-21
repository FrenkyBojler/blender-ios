# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Single load point for the SculptCore engine: imports the ``sculptcore``
ctypes package (``$SCULPTCORE_PYTHON_PATH`` first so a development checkout
overrides the bundle, then the vendored ``lib/`` staged by
``make.mjs bundle``), initializes the binding manager once, and declares
the bulk c-api entry points the conversion layer uses.

The session registry lives here too: one ``session.Session`` per object
currently in the mode, keyed by object name.
"""

import ctypes
import os
import sys

_manager = None
_capi = None

# Object name -> session.Session for every object currently in the mode.
sessions = {}


class EngineError(RuntimeError):
    pass


def _import_sculptcore():
    try:
        import sculptcore
        return sculptcore
    except ImportError:
        pass

    # The dev checkout must win over the vendored bundle, or setting the env
    # var silently stops working once a bundle has been staged.
    candidates = []
    dev_path = os.environ.get("SCULPTCORE_PYTHON_PATH")
    if dev_path:
        candidates.append(dev_path)
    vendored = os.path.join(os.path.dirname(__file__), "lib")
    if os.path.isdir(os.path.join(vendored, "sculptcore")):
        candidates.append(vendored)

    for path in candidates:
        if path not in sys.path:
            sys.path.insert(0, path)
        try:
            import sculptcore
            return sculptcore
        except ImportError:
            continue

    raise EngineError(
        "SculptCore engine not found: vendor it into {:s} or set "
        "SCULPTCORE_PYTHON_PATH to the package directory".format(vendored))


def manager():
    """The engine binding manager (lazy; loads the native library on first
    use and refuses an ABI-mismatched build)."""
    global _manager
    if _manager is None:
        sculptcore = _import_sculptcore()
        _manager = sculptcore.init()
    return _manager


class _CApi:
    """ctypes declarations for the c-api seams the conversion layer calls
    (the bulk-array entry points are free functions, not reflected)."""

    def __init__(self, lib):
        import numpy as np

        f32p = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
        i32p = np.ctypeslib.ndpointer(dtype=np.int32, flags="C_CONTIGUOUS")
        c_int_p = ctypes.POINTER(ctypes.c_int)

        lib.Mesh_fromArrays.argtypes = [
            f32p, ctypes.c_int, i32p, ctypes.c_int, i32p, ctypes.c_int,
        ]
        lib.Mesh_fromArrays.restype = ctypes.c_void_p
        lib.Mesh_arraySizes.argtypes = [ctypes.c_void_p] + [c_int_p] * 4
        lib.Mesh_arraySizes.restype = None
        lib.Mesh_toArrays.argtypes = [ctypes.c_void_p, f32p, i32p, i32p, i32p]
        lib.Mesh_toArrays.restype = ctypes.c_int
        lib.Mesh_topoStamp.argtypes = [ctypes.c_void_p]
        lib.Mesh_topoStamp.restype = ctypes.c_uint64
        lib.Mesh_readVertFloatAttr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, f32p]
        lib.Mesh_readVertFloatAttr.restype = ctypes.c_int
        lib.Mesh_writeVertFloatAttr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, f32p]
        lib.Mesh_writeVertFloatAttr.restype = ctypes.c_int
        lib.Mesh_readFaceIntAttr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, i32p]
        lib.Mesh_readFaceIntAttr.restype = ctypes.c_int
        lib.Mesh_writeFaceIntAttr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, i32p]
        lib.Mesh_writeFaceIntAttr.restype = ctypes.c_int
        lib.Mesh_readVertFloat4Attr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, f32p]
        lib.Mesh_readVertFloat4Attr.restype = ctypes.c_int
        lib.Mesh_writeVertFloat4Attr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, f32p]
        lib.Mesh_writeVertFloat4Attr.restype = ctypes.c_int
        lib.Mesh_writeCornerFloat2Attr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, f32p]
        lib.Mesh_writeCornerFloat2Attr.restype = ctypes.c_int
        # Generic attribute bridge: read/write a named layer of arbitrary type on
        # any domain. The buffer is typed by `type` (an engine AttrType), so it is
        # passed as a raw void pointer (the numpy array carries the real dtype).
        lib.Mesh_readAttr.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
        lib.Mesh_readAttr.restype = ctypes.c_int
        lib.Mesh_writeAttr.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
            ctypes.c_void_p]
        lib.Mesh_writeAttr.restype = ctypes.c_int
        # Boundary edge flags (P11): seam/sharp migration keyed by vertex
        # pairs (the engine derives its own edges, so there is no stable edge
        # index correspondence), plus the boundary recompute and the
        # named-target UV unwrap.
        u8p = np.ctypeslib.ndpointer(dtype=np.uint8, flags="C_CONTIGUOUS")
        lib.Mesh_edgeCount.argtypes = [ctypes.c_void_p]
        lib.Mesh_edgeCount.restype = ctypes.c_int
        lib.Mesh_writeEdgeFlagsByVerts.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, i32p, u8p, ctypes.c_int]
        lib.Mesh_writeEdgeFlagsByVerts.restype = ctypes.c_int
        lib.Mesh_readEdgeFlags.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, i32p, ctypes.c_int]
        lib.Mesh_readEdgeFlags.restype = ctypes.c_int
        lib.Mesh_recomputeBoundary.argtypes = [ctypes.c_void_p]
        lib.Mesh_recomputeBoundary.restype = None
        lib.Mesh_generateUVFromSeams.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        lib.Mesh_generateUVFromSeams.restype = ctypes.c_int
        lib.freeMesh.argtypes = [ctypes.c_void_p]
        lib.freeMesh.restype = None
        lib.Mesh_buildSpatialTree.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 3
        lib.Mesh_buildSpatialTree.restype = ctypes.c_void_p
        lib.SpatialTree_free.argtypes = [ctypes.c_void_p]
        lib.SpatialTree_free.restype = None

        # Multires (P8): stack over a cage, per-level materialization, and the
        # position seed/dump that round-trips a level's surface through MDISPS.
        c_int_p4 = [ctypes.POINTER(ctypes.c_int)] * 4
        lib.Mesh_arraySizes.argtypes = [ctypes.c_void_p] + c_int_p4
        lib.Mesh_arraySizes.restype = None
        lib.Mesh_toArrays.argtypes = [ctypes.c_void_p, f32p, i32p, i32p, i32p]
        lib.Mesh_toArrays.restype = ctypes.c_int
        lib.Multires_new.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 4
        lib.Multires_new.restype = ctypes.c_void_p
        lib.Multires_free.argtypes = [ctypes.c_void_p]
        lib.Multires_free.restype = None
        lib.Multires_setActiveLevel.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.Multires_setActiveLevel.restype = ctypes.c_int
        lib.Multires_activeMesh.argtypes = [ctypes.c_void_p]
        lib.Multires_activeMesh.restype = ctypes.c_void_p
        lib.Multires_activeTree.argtypes = [ctypes.c_void_p]
        lib.Multires_activeTree.restype = ctypes.c_void_p
        lib.Multires_levelSampleCount.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.Multires_levelSampleCount.restype = ctypes.c_int
        lib.Multires_levelPositionsOut.argtypes = [ctypes.c_void_p, ctypes.c_int, f32p]
        lib.Multires_levelPositionsOut.restype = ctypes.c_int
        lib.Multires_fromLevelPositions.argtypes = [
            ctypes.c_void_p, ctypes.c_int, f32p, ctypes.c_int]
        lib.Multires_fromLevelPositions.restype = ctypes.c_int
        lib.Multires_writeback.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.Multires_writeback.restype = ctypes.c_int
        # Store snapshot seam (P8 C4): serialize returns an engine-owned
        # buffer (free with freeMeshBuffer); restore replaces the store and
        # invalidates every derived level (re-activate + re-fetch afterwards).
        lib.Multires_serializeStore.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        lib.Multires_serializeStore.restype = ctypes.POINTER(ctypes.c_uint8)
        lib.Multires_restoreStore.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        lib.Multires_restoreStore.restype = ctypes.c_int
        lib.freeMeshBuffer.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
        lib.freeMeshBuffer.restype = None

        # External draw provider (P5 D6): register a tree under the object's
        # session_uid, refresh its GPU-node CPU buffers, and hand Blender the
        # native provider address.
        lib.sc_external_draw_register.argtypes = [ctypes.c_uint, ctypes.c_void_p]
        lib.sc_external_draw_register.restype = None
        lib.sc_external_draw_unregister.argtypes = [ctypes.c_uint]
        lib.sc_external_draw_unregister.restype = None
        lib.sc_external_draw_update.argtypes = [ctypes.c_uint]
        lib.sc_external_draw_update.restype = None
        lib.sc_external_draw_enable_dynamic.argtypes = [ctypes.c_void_p]
        lib.sc_external_draw_enable_dynamic.restype = None
        lib.sc_external_draw_provider.argtypes = []
        lib.sc_external_draw_provider.restype = ctypes.c_void_p

        self.lib = lib


def capi():
    global _capi
    if _capi is None:
        _capi = _CApi(manager().capi.lib)
    return _capi


def free_all_sessions():
    """Drop every live session (addon unregister; the C side has already
    force-exited the objects and flushed via exit())."""
    for session in list(sessions.values()):
        session.free()
    sessions.clear()
