# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Blender Brush -> SculptCore Brush mapping (declarative table).

M1 slice: the DRAW brush with radius/strength/spacing/invert/falloff. The
table is per Blender ``sculpt_brush_type``; broadening it is additive
(brush-mapping plan §3). World-space radius unprojection is the stroke
operator's job (per view); this layer only copies engine-space fields.
"""

# Blender sculpt_brush_type -> SculptCore SculptBrushes enum name.
# Only mapped types can be sculpted with; the UI greys the rest out.
KERNEL_BY_TYPE = {
    'DRAW': "DRAW",
    'CLAY': "CLAY",
    'INFLATE': "INFLATE",
    'PINCH': "PINCH",
    'SMOOTH': "SMOOTH",
    'SCRAPE': "SCRAPE",
    'FILL': "FILL",
    'GRAB': "GRAB",
    'SNAKE_HOOK': "SNAKEHOOK",
    'MASK': "MASK",
    'DRAW_SHARP': "SHARP",
    'LAYER': "LAYERDRAW",
}


def kernel_enum(mgr, bl_brush):
    """The SculptBrushes enum value for a Blender brush, or None when the
    brush type has no SculptCore kernel."""
    name = KERNEL_BY_TYPE.get(bl_brush.sculpt_brush_type)
    if name is None:
        return None
    return int(mgr.get("sculptcore::brush::SculptBrushes").items[name])


def apply_brush(bl_brush, unified, sc_brush, *, world_radius, invert):
    """Configure a SculptCore Brush from a Blender Brush for a stroke.

    ``world_radius`` is the object-space dab radius (the stroke operator
    unprojects the pixel size per view); ``invert`` folds the brush direction
    flag with a live modifier (e.g. Ctrl). ``unified`` is the per-Paint
    ``UnifiedPaintSettings`` for size/strength overrides (may be None).
    """
    strength = bl_brush.strength
    if unified is not None and unified.use_unified_strength:
        strength = unified.strength

    sc_brush.strength = strength
    sc_brush.radius = world_radius
    sc_brush.spacing = max(bl_brush.spacing, 1) / 100.0  # percent -> fraction
    sc_brush.invert = bool(invert) ^ bool(bl_brush.direction == 'SUBTRACT')

    # writeProps() bakes the scalar fields into the kernel's uniform block.
    sc_brush.writeProps()
