# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tier-2 delta undo: couple Blender's ``CUSTOM_MODE`` undo steps to the engine's
per-session ``MeshLog``. Each stroke pushes one step (:func:`push`); Blender
drives one meshlog undo/redo per step transition (:func:`decode`); an evicted
step frees its meshlog entry (:func:`free`).

The ``CUSTOM_MODE`` undo type decodes the *active* step as well as the
destination (``UNDOTYPE_FLAG_DECODE_ACTIVE_STEP``), so on undo Blender calls us
for the step being left (``is_final`` false) and then the destination
(``is_final`` true); on redo it calls us for each step entered. Each pushed step
records the meshlog applied-step count with it applied (``target``, mirroring
``curStep_``). :func:`decode` *seeks* the meshlog cursor to the right count:

  * undo, leaving this step (not final): seek to ``target - 1`` (revert it).
  * undo, landing on this step (final): seek to ``target`` (keep it applied).
  * redo (entering this step): seek to ``target``.

Seeking (rather than a single step) makes the cursor robust even across a
memfile boundary that decodes to a memfile step this type never sees. The
Blender-side ``state_id`` is a global key routing :func:`decode`/:func:`free`
to the right session; the session generation detects a rebuilt session.

Multires sessions (P8 C4) additionally snapshot the displacement store with
each step (``blob_before``/``blob_after`` — consecutive steps share one bytes
object). When the meshlog seek is unavailable — a level switch or an earlier
blob restore reset the meshlog and bumped the generation — decode falls back
to restoring the step's blob at its recorded level, which is exact because
every stroke's edits are written back into the store at push time. The level
switch itself needs no step here: the ``sculpt_levels`` property edit pushes
a memfile step, and undoing that re-drives the engine through the depsgraph
handler.
"""

import bpy

from . import convert, engine

# Global step key -> (object_name, meshlog_step_id, target_cursor, generation,
# blob_before, blob_after, level). Keyed globally because undo_free() receives
# only the key (no object). target is the meshlog applied-step count when the
# mesh is at this step; generation detects a session rebuilt out from under
# these keys. The blobs/level are None/0 for plain-Mesh sessions.
_pending = {}
_next_key = 1

# Sentinel tagging attribute-snapshot steps in _pending (identity-compared, so
# an object named "ATTR" can't collide). Attr steps: (tag, object_name,
# generation, kind, attr_name_bytes, blob_before, blob_after) — the op wrote
# an engine column directly (no meshlog entry), so decode restores the right
# snapshot instead of seeking (see push_attr). ``kind`` selects the column
# domain/type; each entry is (numpy dtype, count-getter, writer-getter).
_ATTR_TAG = ("attr",)

_ATTR_KINDS = {
    'VERT_F32': ("float32",
                 lambda session: convert.mesh_vert_num(session.mesh_ptr),
                 lambda lib: lib.Mesh_writeVertFloatAttr),
    'FACE_I32': ("int32",
                 lambda session: convert.mesh_face_num(session.mesh_ptr),
                 lambda lib: lib.Mesh_writeFaceIntAttr),
    # Corner FLOAT2 (UV) columns: len(values) is floats, so 2 per corner.
    'CORNER_F32x2': ("float32",
                     lambda session: convert.mesh_corner_num(session.mesh_ptr) * 2,
                     lambda lib: lib.Mesh_writeCornerFloat2Attr),
}


def _tag_view3d_redraw(context):
    window_manager = (context or bpy.context).window_manager
    for window in window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


def push(context, ob, session):
    """Record one undo step for the stroke just ended on ``ob``'s session."""
    global _next_key
    log = session.meshlog
    if log is None:
        return
    step_id = int(log.lastStepId())
    if step_id < 0:
        return
    size = int(log.stepMemSize(step_id))
    blob_before = blob_after = None
    level = 0
    if session.multires_ptr:
        # C4: snapshot the store (post-writeback, so this stroke is included);
        # the previous snapshot is this step's pre-state. Each blob is shared
        # with the neighbouring step, so count it once for the undo limiter.
        blob_before = session.multires_last_blob
        blob_after = convert.multires_store_blob(session)
        session.multires_last_blob = blob_after
        level = session.multires_active_level
        if blob_after is not None:
            size += len(blob_after)
    key = _next_key
    _next_key += 1
    # target = applied-step count with this stroke applied (mirrors curStep_).
    _pending[key] = (ob.name, step_id, session.meshlog_cursor, session.generation,
                     blob_before, blob_after, level)
    bpy.ops.object.custom_mode_undo_push(
        'EXEC_DEFAULT', message="Sculpt Stroke", state_id=key, size=size)


def push_attr(context, ob, session, message, kind, attr, blob_before, blob_after):
    """Record one undo step for a whole-column attribute write (e.g. mask
    flood fill). The engine meshlog never sees such writes, so the step
    carries before/after snapshots; decode restores them. ``kind`` is an
    _ATTR_KINDS key, ``attr`` the engine column name (bytes), blobs the raw
    column bytes."""
    global _next_key
    key = _next_key
    _next_key += 1
    _pending[key] = (_ATTR_TAG, ob.name, session.generation,
                     kind, attr, blob_before, blob_after)
    bpy.ops.object.custom_mode_undo_push(
        'EXEC_DEFAULT', message=message, state_id=key,
        size=len(blob_before) + len(blob_after))


def _decode_attr(context, ob, session, info, direction, is_final):
    """Restore an attribute snapshot. Leaving a step on undo restores its
    pre-state; landing on one (or entering on redo) restores its post-state.
    A length mismatch (topology diverged from this step's state, e.g. an
    out-of-order decode around dyntopo steps) skips the restore rather than
    corrupting the column."""
    import numpy as np

    _tag, _name, generation, kind, attr, blob_before, blob_after = info
    if generation != session.generation:
        return
    blob = blob_before if (direction < 0 and not is_final) else blob_after
    dtype, count_fn, writer_fn = _ATTR_KINDS[kind]
    values = np.frombuffer(blob, dtype=dtype)
    if count_fn(session) != len(values):
        return
    writer_fn(engine.capi().lib)(session.mesh_ptr, attr, np.ascontiguousarray(values))
    # Flush on the leave decode too, not only on the final one: an undo whose
    # destination is a step this type never decodes (e.g. the mode-enter
    # memfile boundary) would otherwise leave the restored column engine-only,
    # with the Mesh still showing the undone state.
    convert.flush(ob)
    if is_final:
        _tag_view3d_redraw(context)


def _decode_multires_blob(context, ob, session, info, direction, is_final):
    """C4 fallback: the step's meshlog is gone (level switch / blob restore),
    so restore its store snapshot. Leaving a step on undo restores its
    pre-state; landing on a step (or entering it on redo) restores its
    post-state. Neighbouring steps share blobs, so a leave + land pair on the
    same history edge restores the same bytes (idempotent)."""
    _name, _step_id, _target, _generation, blob_before, blob_after, level = info
    blob = blob_before if (direction < 0 and not is_final) else blob_after
    if blob is None:
        return
    if convert.multires_restore_blob(ob, session, blob, level):
        convert.flush(ob)
        _tag_view3d_redraw(context)


def decode(context, ob, state_id, direction, is_final):
    """Seek the meshlog cursor to the step's target for ``ob``. On undo the
    step being left (not final) seeks to ``target - 1``; the destination
    (final) and any redo seek to ``target``. Multires steps whose meshlog
    died fall back to the store-snapshot restore."""
    session = engine.sessions.get(ob.name)
    if session is None:
        # Session gone (mode exited): the mesh is restored by memfile/refresh,
        # so there is nothing to replay at the engine level.
        return
    info = _pending.get(state_id)
    if info is None:
        return
    if info[0] is _ATTR_TAG:
        _decode_attr(context, ob, session, info, direction, is_final)
        return
    _object_name, _step_id, target, generation, _blob_before, blob_after, _level = info
    if session.multires_ptr and blob_after is not None and (
            generation != session.generation or session.meshlog is None):
        _decode_multires_blob(context, ob, session, info, direction, is_final)
        return
    if session.meshlog is None:
        return
    if generation != session.generation:
        # A foreign memfile decode rebuilt the session; this older step no
        # longer maps onto its fresh history. The memfile decode already
        # restored the mesh, so this is an engine-level no-op.
        return
    if direction < 0 and not is_final:
        # Undo leaving this step: drop below it.
        target -= 1
    log = session.meshlog
    mesh = session.mesh()
    tree = session.tree()
    moved = False
    # Seek the cursor to `target`: undo down / redo up. Guarded against the
    # meshlog's own bounds so an evicted-past target stops cleanly.
    while session.meshlog_cursor > target and session.meshlog_cursor > 0:
        log.undo(mesh, tree)
        session.meshlog_cursor -= 1
        moved = True
    while session.meshlog_cursor < target:
        log.redo(mesh, tree)
        session.meshlog_cursor += 1
        moved = True
    if moved:
        mesh.recalc_normals()
    # Flush on every final decode even when the cursor did not move: the undo
    # system may have decoded an older memfile below this step first (the
    # correct-order rule), replacing the Mesh data the engine no longer
    # matches. The flush re-asserts the engine state onto the Mesh.
    if moved or is_final:
        if session.multires_ptr:
            # Keep the C4 blob chain rooted at the landed state so a stroke
            # begun after this undo/redo records the right pre-state.
            _bb, _ba = info[4], info[5]
            landed = _ba if (is_final or direction > 0) else _bb
            if landed is not None:
                session.multires_last_blob = landed
        convert.flush(ob)
        _tag_view3d_redraw(context)


def free(state_id):
    """Drop the meshlog entry for an evicted undo step (the store blobs go
    with the popped registry entry)."""
    info = _pending.pop(state_id, None)
    if info is None:
        return
    if info[0] is _ATTR_TAG:
        # No meshlog entry to free; the snapshots go with the popped entry.
        return
    object_name, step_id, _target, generation, _bb, _ba, _level = info
    session = engine.sessions.get(object_name)
    if (session is not None and session.meshlog is not None
            and session.generation == generation):
        session.meshlog.freeStep(step_id)


def reset():
    """Forget all pending step keys (addon unregister / full reload)."""
    _pending.clear()
