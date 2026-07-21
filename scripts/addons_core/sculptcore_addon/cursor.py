# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Viewport brush cursor: a wire circle at the mouse with the brush's pixel
radius, drawn through the mode's ``draw_cursor`` callback (the WM
paint-cursor mechanism — the region redraws on every mouse move while the
mode is active; coordinates arrive in region pixel space).
"""

from . import engine

_shader = None
_batch = None
_failed = False

# Live size-pressure factor published by the stroke operator so the cursor
# circle tracks the pen the same way the deformation does; 1.0 when idle or
# when size pressure is off.
_size_scale = 1.0


def set_size_scale(scale):
    """Set the cursor radius multiplier (the stroke operator's size-pressure
    factor). Reset to 1.0 when a stroke ends."""
    global _size_scale
    _size_scale = scale


def _ensure_batch():
    global _shader, _batch
    if _batch is None:
        import math

        import gpu
        from gpu_extras.batch import batch_for_shader

        _shader = gpu.shader.from_builtin('UNIFORM_COLOR')
        step = 2.0 * math.pi / 40
        points = [(math.cos(i * step), math.sin(i * step), 0.0) for i in range(40)]
        _batch = batch_for_shader(_shader, 'LINE_LOOP', {"pos": points})
    return _batch


def draw(context, x, y):
    """Draw the cursor circle; radius follows the (unified) brush pixel size,
    color the brush's cursor color. No-op without a live session or brush."""
    global _failed
    if _failed:
        return
    ob = context.active_object
    if ob is None or ob.name not in engine.sessions:
        return
    sculpt = context.tool_settings.sculpt
    brush = sculpt.brush
    if brush is None:
        return
    unified = sculpt.unified_paint_settings
    radius = (unified.size if unified.use_unified_size else brush.size) * _size_scale

    try:
        import gpu

        batch = _ensure_batch()
        color = tuple(brush.cursor_color_add[:3]) + (0.9,)
        gpu.state.blend_set('ALPHA')
        gpu.state.line_width_set(2.0)
        with gpu.matrix.push_pop():
            gpu.matrix.translate((x, y, 0.0))
            gpu.matrix.scale_uniform(float(radius))
            _shader.uniform_float("color", color)
            batch.draw(_shader)
        gpu.state.line_width_set(1.0)
        gpu.state.blend_set('NONE')
    except Exception:
        # A broken draw would re-raise on every redraw; report once and stop.
        import traceback

        traceback.print_exc()
        print("SculptCore: cursor draw failed; overlay disabled")
        _failed = True
