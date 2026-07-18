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
    object_name, step_id, _target, generation, _bb, _ba, _level = info
    session = engine.sessions.get(object_name)
    if (session is not None and session.meshlog is not None
            and session.generation == generation):
        session.meshlog.freeStep(step_id)


def reset():
    """Forget all pending step keys (addon unregister / full reload)."""
    _pending.clear()
