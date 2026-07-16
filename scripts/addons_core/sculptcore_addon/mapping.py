# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Blender Brush -> SculptCore Brush mapping (declarative table).

M1: the per-dab sculpting brushes. Each entry names the SculptCore kernel
and (optionally) per-type field values applied on stroke start. World-space
radius unprojection is the stroke operator's job; this layer copies
engine-space fields only.

Grab-family and layer brushes are intentionally *not* supported yet: grab/
snake-hook/pose need per-stroke anchor/delta state a bare per-dab
``execBrush`` doesn't set up (and crash without it), and layer needs a
persistent sculpt-layer attribute + brush texture. ``kernel_enum`` returns
None for those so the stroke operator refuses cleanly rather than crashing.
"""

# Blender sculpt_brush_type -> (SculptCore SculptBrushes name, extra fields).
# `extra` is a dict of SculptCore Brush field -> value/callable(bl_brush).
# Verified per-dab via the parity harness (test_brush_parity).
_MAP = {
    'DRAW': ("DRAW", {}),
    'DRAW_SHARP': ("SHARP", {}),
    'INFLATE': ("INFLATE", {}),
    # Clay + plane family: map the plane offset; the default plane side (+1)
    # produces sensible output at a convex surface (a -1 scrape side finds
    # nothing above the tangent plane on a sphere). Blender's 'PLANE' is the
    # unified flatten/fill brush and 'MULTIPLANE_SCRAPE' the scrape; precise
    # per-mode side/offset semantics is a later refinement.
    'CLAY': ("CLAY", {"planeoff": lambda b: b.plane_offset}),
    'CLAY_STRIPS': ("CLAY", {"planeoff": lambda b: b.plane_offset}),
    'PLANE': ("FILL", {"planeoff": lambda b: b.plane_offset}),
    'MULTIPLANE_SCRAPE': ("SCRAPE", {"planeoff": lambda b: b.plane_offset}),
    'SMOOTH': ("SMOOTH", {}),
    'PINCH': ("PINCH", {"pinch": lambda b: b.strength}),
    'MASK': ("MASK", {}),
    # Face sets: paint the `group` face attr; the stroke operator assigns a
    # fresh active group id per stroke (see FACE_SET_TYPES).
    'DRAW_FACE_SETS': ("POLYGROUP", {}),
    # Snake hook drags per dab at the cursor — the standard path works.
    'SNAKE_HOOK': ("SNAKEHOOK", {}),
    # Grab dabs at a fixed anchor and reads the cumulative cursor delta
    # (grabTo/grabFrom); the stroke operator drives it via the grab-class path.
    'GRAB': ("GRAB", {}),
}

# Brush types that dab at the stroke anchor with a cursor-delta (grabTo)
# instead of at the moving cursor.
GRAB_CLASS = {'GRAB'}

# Brush types that paint face sets — the operator assigns a fresh `activeGroup`
# id (max existing + 1) at stroke start.
FACE_SET_TYPES = {'DRAW_FACE_SETS'}

# Kernels that exist but need infrastructure not wired yet — kept for
# reference / a future UI "unsupported" hint, never entered.
UNSUPPORTED = {
    'POSE': "needs the pose-cage anchor path",
    'LAYER': "needs a sculpt-layer attribute + brush texture",
}


def is_grab_class(bl_brush):
    return bl_brush is not None and bl_brush.sculpt_brush_type in GRAB_CLASS

# For UI / diagnostics: every mapped type (supported or not).
KERNEL_BY_TYPE = {t: v[0] for t, v in _MAP.items()}


def is_supported(bl_brush):
    return bl_brush is not None and bl_brush.sculpt_brush_type in _MAP


def kernel_enum(mgr, bl_brush):
    """The SculptBrushes enum value for a Blender brush, or None when the
    brush type is not supported for sculpting yet."""
    entry = _MAP.get(bl_brush.sculpt_brush_type)
    if entry is None:
        return None
    return int(mgr.get("sculptcore::brush::SculptBrushes").items[entry[0]])


def apply_brush(bl_brush, unified, sc_brush, *, world_radius, invert):
    """Configure a SculptCore Brush from a Blender Brush for a stroke.

    ``world_radius`` is the object-space dab radius; ``invert`` folds a live
    modifier (e.g. Ctrl) with the brush direction flag; ``unified`` is the
    per-Paint ``UnifiedPaintSettings`` (may be None).
    """
    strength = bl_brush.strength
    if unified is not None and unified.use_unified_strength:
        strength = unified.strength

    sc_brush.strength = strength
    sc_brush.radius = world_radius
    sc_brush.spacing = max(bl_brush.spacing, 1) / 100.0  # percent -> fraction
    sc_brush.invert = bool(invert) ^ bool(bl_brush.direction == 'SUBTRACT')

    entry = _MAP.get(bl_brush.sculpt_brush_type)
    if entry is not None:
        for field, value in entry[1].items():
            setattr(sc_brush, field, value(bl_brush) if callable(value) else value)

    # writeProps() bakes the scalar fields into the kernel's uniform block.
    sc_brush.writeProps()
