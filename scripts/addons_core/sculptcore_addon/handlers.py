# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Session lifecycle handlers.

Tier-1 undo is memfile-based, so an undo/redo step can move an object out of
the mode without going through the exit callback — e.g. undoing across the
mode-enter boundary drops the object back to Object mode. The engine session
then dangles (its C++ mesh/tree leak, and a later re-enter would overwrite
it). ``undo_post``/``redo_post`` reconcile the session registry against the
objects' actual modes; ``load_post`` drops every session (the engine meshes
were built from the previous file's data, now replaced).

The full custom undo type (undo-integration plan) makes stroke undo exact;
this keeps Tier-1 leak-free and consistent in the meantime.
"""

import bpy
from bpy.app.handlers import persistent

from . import engine


def _reconcile():
    """Free any session whose object is gone or no longer in the mode."""
    for name in list(engine.sessions):
        ob = bpy.data.objects.get(name)
        in_mode = (
            ob is not None
            and ob.mode == 'CUSTOM'
            and ob.custom_mode == "sculptcore.sculpt"
        )
        if not in_mode:
            engine.sessions.pop(name).free()


@persistent
def _on_undo_redo(scene, depsgraph=None):
    _reconcile()


@persistent
def _on_load(*_args):
    # The previous file's engine meshes are orphaned by the load.
    engine.free_all_sessions()


def register():
    bpy.app.handlers.undo_post.append(_on_undo_redo)
    bpy.app.handlers.redo_post.append(_on_undo_redo)
    bpy.app.handlers.load_post.append(_on_load)


def unregister():
    for handler_list, fn in (
        (bpy.app.handlers.undo_post, _on_undo_redo),
        (bpy.app.handlers.redo_post, _on_undo_redo),
        (bpy.app.handlers.load_post, _on_load),
    ):
        if fn in handler_list:
            handler_list.remove(fn)
