# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Single load point for the SculptCore engine: imports the ``sculptcore``
ctypes package (vendored ``lib/`` first, then ``$SCULPTCORE_PYTHON_PATH``
for development checkouts), initializes the binding manager once, and
declares the bulk c-api entry points the conversion layer uses.

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

    candidates = []
    vendored = os.path.join(os.path.dirname(__file__), "lib")
    if os.path.isdir(os.path.join(vendored, "sculptcore")):
        candidates.append(vendored)
    dev_path = os.environ.get("SCULPTCORE_PYTHON_PATH")
    if dev_path:
        candidates.append(dev_path)

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
        lib.freeMesh.argtypes = [ctypes.c_void_p]
        lib.freeMesh.restype = None
        lib.Mesh_buildSpatialTree.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 3
        lib.Mesh_buildSpatialTree.restype = ctypes.c_void_p
        lib.SpatialTree_free.argtypes = [ctypes.c_void_p]
        lib.SpatialTree_free.restype = None

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
