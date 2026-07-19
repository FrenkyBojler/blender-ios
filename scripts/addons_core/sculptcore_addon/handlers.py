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

``depsgraph_update_post`` additionally follows the multires modifier's
``sculpt_levels`` so a change in the modifier UI switches the session's
active engine level (P8 C2).
"""

import bpy
from bpy.app.handlers import persistent

from . import engine, multires


def _tag_view3d_redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


def _sync_multires_levels():
    """Follow the multires modifier's sculpt level (C2): when the user moves
    ``sculpt_levels`` in the modifier UI, switch the session's active engine
    level. Cheap when nothing changed (a dict scan and an int compare)."""
    from . import convert

    for name in list(engine.sessions):
        session = engine.sessions[name]
        if not session.multires_ptr:
            continue
        ob = bpy.data.objects.get(name)
        if ob is None:
            continue
        md = multires.modifier(ob)
        if md is None:
            continue
        want = min(max(md.sculpt_levels, 1), session.multires_level)
        if want != session.multires_active_level:
            convert.set_multires_level(ob, want)
            _tag_view3d_redraw()


@persistent
def _on_depsgraph_update(scene, depsgraph=None):
    # Deleting an in-mode object (outliner / bpy.data) never runs the exit
    # callback and fires no undo signal until much later, so reconcile here
    # too (cheap: a dict scan) or its engine session leaks.
    _reconcile()
    _sync_multires_levels()
    if depsgraph is not None:
        from . import texture
        texture.invalidate_from_depsgraph(depsgraph)


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
    # The previous file's engine meshes are orphaned by the load, and the
    # texture bakes belong to the replaced file's datablocks.
    engine.free_all_sessions()
    from . import texture
    texture.invalidate()


def register():
    bpy.app.handlers.undo_post.append(_on_undo_redo)
    bpy.app.handlers.redo_post.append(_on_undo_redo)
    bpy.app.handlers.load_post.append(_on_load)
    bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph_update)


def unregister():
    for handler_list, fn in (
        (bpy.app.handlers.undo_post, _on_undo_redo),
        (bpy.app.handlers.redo_post, _on_undo_redo),
        (bpy.app.handlers.load_post, _on_load),
        (bpy.app.handlers.depsgraph_update_post, _on_depsgraph_update),
    ):
        if fn in handler_list:
            handler_list.remove(fn)
