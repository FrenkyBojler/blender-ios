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

# Engine constants for the device-dynamics seam (brush.h BrushProp ids,
# prop_dynamics.h DeviceType, litestl mix.h BasicMix). The string-keyed
# dynamics API is unreachable from Python (util::string args), so the stroke
# operator configures pressure through these int-keyed ids.
PROP_STRENGTH = 0
PROP_RADIUS = 1
DEVICE_PRESSURE = 0
MIX_MULTIPLY = 1

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
    # BSMOOTH (boundary-aware smooth): identical to plain SMOOTH on meshes
    # with no marked feature edges. Seam/sharp edge flags transfer to the
    # engine on enter (convert._load_edge_flags), so marked features hold
    # under smoothing (verified: a sharp-marked crest erodes 0% vs 66% of its
    # height unmarked — claudeMemory/tests/bsmooth_boundary_test.py).
    'SMOOTH': ("BSMOOTH", {}),
    'PINCH': ("PINCH", {"pinch": lambda b: b.strength}),
    'MASK': ("MASK", {}),
    # Vertex paint: brushColor synced from the Blender brush color (see
    # apply_brush); writes the `color` float4 vertex attr.
    'PAINT': ("COLOR", {}),
    # Face sets: paint the `group` face attr; the stroke operator assigns a
    # fresh active group id per stroke (see FACE_SET_TYPES).
    'DRAW_FACE_SETS': ("POLYGROUP", {}),
    # Snake hook drags per dab at the cursor — the standard path works.
    'SNAKE_HOOK': ("SNAKEHOOK", {}),
    # Grab dabs at a fixed anchor and reads the cumulative cursor delta
    # (grabTo/grabFrom); the stroke operator drives it via the grab-class path.
    'GRAB': ("GRAB", {}),
    # Elastic deform = a Kelvinlet soft-body grab (same grabFrom/grabTo state,
    # engine mu/nu defaults ~ soft rubber).
    'ELASTIC_DEFORM': ("KELVINLET", {}),
}

# Brush types that dab at the stroke anchor with a cursor-delta (grabTo)
# instead of at the moving cursor.
GRAB_CLASS = {'GRAB', 'ELASTIC_DEFORM'}

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


# SculptCore FalloffKind / FalloffShape enum values (brush.h).
_FALLOFF_KIND_CURVE = 3
_FALLOFF_SHAPE_SPHERICAL = 0
_FALLOFF_CURVE_SIZE = 256


# Closed-form preset falloffs, keyed by `curve_distance_falloff_preset`. `t`
# is 1 - normalized distance (1 at center, 0 at edge) — the same value the
# engine feeds falloffEval. Mirrors BKE_brush_curve_strength so brush feel
# matches Blender exactly; CUSTOM samples the editable CurveMapping instead.
_PRESET_FALLOFF = {
    'SHARP': lambda t: t * t,
    'SMOOTH': lambda t: 3.0 * t * t - 2.0 * t * t * t,
    'SMOOTHER': lambda t: t * t * t * (t * (t * 6.0 - 15.0) + 10.0),
    'ROOT': lambda t: t ** 0.5,
    'LIN': lambda t: t,
    'CONSTANT': lambda t: 1.0,
    'SPHERE': lambda t: max(0.0, 2.0 * t - t * t) ** 0.5,
    'POW4': lambda t: t * t * t * t,
    'INVSQUARE': lambda t: t * (2.0 - t),
}


def _bake_falloff(bl_brush, sc_brush):
    """Bake the Blender falloff (preset formula, or the editable curve for
    CUSTOM) into the engine's 256-entry LUT, folding in `hardness`, so brush
    feel matches Blender. `t` = 1 - normalized distance (the value the engine
    feeds falloffEval); hardness remaps the distance before the falloff so the
    inner `hardness` fraction reads full strength."""
    fn = _PRESET_FALLOFF.get(bl_brush.curve_distance_falloff_preset)
    if fn is None:  # CUSTOM: strength(p) = curve(1 - p), matching BKE.
        cumap = bl_brush.curve_distance_falloff
        cumap.update()
        curve = cumap.curves[0]
        def fn(p, _c=cumap, _cv=curve):
            return _c.evaluate(_cv, 1.0 - p)

    hardness = min(1.0, max(0.0, bl_brush.hardness))
    n = _FALLOFF_CURVE_SIZE
    for i in range(n):
        t = i / (n - 1)
        d = 1.0 - t  # normalized distance
        if hardness >= 1.0:
            v = 1.0 if d < 1.0 else 0.0  # hard disc
        elif hardness > 0.0:
            v = 1.0 if d < hardness else fn(1.0 - (d - hardness) / (1.0 - hardness))
        else:
            v = fn(t)
        sc_brush.setFalloffCurveEntry(i, min(1.0, max(0.0, v)))
    sc_brush.falloff_kind = _FALLOFF_KIND_CURVE
    # PROJECTED (2D view falloff) has no distinct engine metric yet; both use
    # the spherical distance for now.
    sc_brush.falloff_shape = _FALLOFF_SHAPE_SPHERICAL


# Cavity automasking. The engine mirrors Blender's estimator and remap
# (automask.h, ported from `calc_cavity_factor`), so the mapping is a direct
# field copy plus the optional custom curve baked into the engine's LUT.
_CAVITY_CURVE_SIZE = 256


def cavity_settings(bl_brush, paint):
    """The `MeshAutomaskingSettings` that governs cavity for this stroke, or
    None when cavity automasking is off.

    Mirrors Blender's `automasking_flags_get` precedence: the brush's own
    cavity flags win when it enables either cavity mode, otherwise the
    Paint-level settings apply."""
    for settings in (bl_brush.mesh_automasking_settings if bl_brush else None,
                     paint.mesh_automasking_settings if paint else None):
        if settings is not None and (settings.use_automasking_cavity
                                     or settings.use_automasking_cavity_inverted):
            return settings
    return None


def _apply_cavity(settings, sc_brush):
    """Copy the resolved cavity settings onto the engine brush. The executor
    pre-fills a per-vertex factor once per stroke when `automask_cavity` is
    set; every kernel that reads the strength intrinsic is masked by it."""
    if settings is None:
        sc_brush.automask_cavity = False
        return

    sc_brush.automask_cavity = True
    sc_brush.cavity_inverted = bool(settings.use_automasking_cavity_inverted)
    sc_brush.cavity_factor = settings.cavity_factor
    sc_brush.cavity_blur_steps = settings.cavity_blur_steps

    use_curve = bool(settings.use_automasking_custom_cavity_curve)
    sc_brush.cavity_use_curve = use_curve
    if not use_curve:
        return
    # The engine samples the LUT in un-inverted space and inverts afterwards,
    # the same order Blender evaluates the curve in, so bake it as authored.
    cumap = settings.cavity_curve
    cumap.update()
    curve = cumap.curves[0]
    n = _CAVITY_CURVE_SIZE
    for i in range(n):
        value = cumap.evaluate(curve, i / (n - 1))
        sc_brush.setCavityCurveEntry(i, min(1.0, max(0.0, value)))


# Pen-pressure response curves. Blender maps tablet pressure to a strength /
# size factor through the Brush.curve_strength / Brush.curve_size
# CurveMappings (vanilla BKE_curvemapping_evaluateF(curve, 0, pressure)). The
# engine mirrors this with a per-device response table (prop_dynamics.h
# DynamicDevice.curveTable); the smooth brush folds pressure in Python-side, so
# the same table is also sampled directly. Baked once per stroke, never per dab.
_PRESSURE_CURVE_SIZE = 256


def sample_pressure_curve(cumap):
    """Sample a Blender pressure CurveMapping into a list mapping pressure
    (0..1, in ``_PRESSURE_CURVE_SIZE`` steps) to a response factor, matching
    vanilla's ``BKE_curvemapping_evaluateF(curve, 0, pressure)``."""
    cumap.update()
    curve = cumap.curves[0]
    n = _PRESSURE_CURVE_SIZE
    return [cumap.evaluate(curve, i / (n - 1)) for i in range(n)]


def eval_pressure_lut(lut, pressure):
    """Look up a pressure factor in a table from ``sample_pressure_curve``,
    using the same clamped linear interpolation as the engine's ``deviceFactor``
    so the Python-side (smooth) and engine-side paths agree."""
    n = len(lut)
    x = min(1.0, max(0.0, pressure)) * (n - 1)
    i = int(x)
    if i >= n - 1:
        return lut[n - 1]
    t = x - i
    return lut[i] * (1.0 - t) + lut[i + 1] * t


def apply_pressure_dynamics(bl_brush, sc_brush, *, use_strength, use_size):
    """Configure the engine's per-stroke pressure dynamics: a MULTIPLY device
    layer per pressure-enabled channel, carrying the baked response curve from
    the matching Brush CurveMapping. Runs once per stroke (the 256-sample bakes
    are far too slow per dab); the stroke operator refills the device sample
    with the event pressure each dab. The clears always run so a channel toggled
    off — or a grab-class stroke that passes both flags false — leaves no stale
    dynamic behind."""
    sc_brush.clearPropDynamics(PROP_STRENGTH)
    sc_brush.clearPropDynamics(PROP_RADIUS)
    if use_strength:
        _add_pressure_dynamic(sc_brush, PROP_STRENGTH, bl_brush.curve_strength)
    if use_size:
        _add_pressure_dynamic(sc_brush, PROP_RADIUS, bl_brush.curve_size)


def _add_pressure_dynamic(sc_brush, prop_id, cumap):
    sc_brush.addPropDynamic(prop_id, DEVICE_PRESSURE, MIX_MULTIPLY, 1.0)
    table = sample_pressure_curve(cumap)
    n = len(table)
    for i, value in enumerate(table):
        sc_brush.setPropDynamicSample(prop_id, DEVICE_PRESSURE, i, n, value)


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


def apply_brush_settings(bl_brush, unified, sc_brush, *, paint=None):
    """Configure the stroke-constant part of a SculptCore Brush from a
    Blender Brush: the scalar settings, per-type extras, and the falloff /
    cavity curve bakes. The bakes are 256 engine calls each (~3-5 ms), far
    too slow for the per-dab path, and none of their inputs can change while
    a stroke is running — so this runs once at stroke start and
    ``apply_dab_state`` writes the per-dab values on top.

    ``unified`` is the per-Paint ``UnifiedPaintSettings`` (may be None).
    ``paint`` is the owning ``Paint`` (``tool_settings.sculpt``), consulted
    for automasking settings the brush itself does not override; without it
    only the brush's own settings apply.
    """
    strength = bl_brush.strength
    if unified is not None and unified.use_unified_strength:
        strength = unified.strength

    sc_brush.strength = strength
    sc_brush.spacing = max(bl_brush.spacing, 1) / 100.0  # percent -> fraction

    entry = _MAP.get(bl_brush.sculpt_brush_type)
    if entry is not None:
        for field, value in entry[1].items():
            setattr(sc_brush, field, value(bl_brush) if callable(value) else value)

    if bl_brush.sculpt_brush_type == 'PAINT':
        col = bl_brush.color  # linear RGB
        bc = sc_brush.brushColor.vec
        bc[0], bc[1], bc[2], bc[3] = col[0], col[1], col[2], 1.0

    _bake_falloff(bl_brush, sc_brush)
    _apply_cavity(cavity_settings(bl_brush, paint), sc_brush)

    # Generated engine-only uniforms (Brush.sculptcore, brush-mapping M2) —
    # after the mapping so table-driven fields keep authority.
    from . import engine_props
    engine_props.apply(bl_brush, sc_brush)


def overlap_attenuation(bl_brush):
    """Vanilla's "Adjust Strength for Spacing"
    (#paint_stroke_integrate_overlap): normalize the strength by the
    worst-case sum of overlapping falloff dabs along the stroke line,
    sampled at 10 phase offsets. 1.0 when the flag is off or spacing has no
    overlap (>= 100%)."""
    if not (bl_brush.use_space_attenuation and bl_brush.spacing < 100):
        return 1.0
    fn = _PRESET_FALLOFF.get(bl_brush.curve_distance_falloff_preset)
    if fn is None:  # CUSTOM: strength(p) = curve(1 - p), matching BKE.
        cumap = bl_brush.curve_distance_falloff
        cumap.update()
        curve = cumap.curves[0]

        def fn(t, _c=cumap, _cv=curve):
            return _c.evaluate(_cv, 1.0 - t)

    spacing = max(bl_brush.spacing, 0.1)
    count = int(100 / spacing)
    h = spacing / 50.0
    peak = 0.0
    for i in range(10):
        x0 = i / 10.0 - 1.0
        total = 0.0
        for j in range(count):
            xx = abs(x0 + j * h)
            if xx < 1.0:
                total += fn(1.0 - xx)
        peak = max(peak, abs(total))
    return 1.0 / peak if peak > 0.0 else 1.0


def apply_dab_state(bl_brush, unified, sc_brush, *, world_radius, invert,
                    strength_scale=1.0, strength_override=None,
                    allow_invert=True):
    """Write the per-dab brush state: strength, radius and the invert flag
    (a live Ctrl toggles it mid-stroke), folded with the brush direction.
    Assumes ``apply_brush_settings`` ran at stroke start. ``strength_scale``
    folds per-stroke factors in (overlap attenuation). ``allow_invert=False``
    forces the invert flag off regardless of Ctrl/direction — smoothing has no
    inverse (the engine negates kernel strength on invert, and an inverted
    Laplacian moves verts away from their ring average, diverging within a few
    dabs), matching vanilla, where smooth ignores the direction.

    Strength and radius must be rewritten every dab, not only at stroke
    start: the engine's per-dab ``loadProps`` assigns the post-dynamics
    values (e.g. strength x pen pressure) back into the Brush *fields*, so
    the ``writeProps`` below would otherwise persist the decayed field into
    the prop store and a pressure stroke would fade to nothing after the
    first dab. Radius varies per dab anyway (depth-dependent unproject)."""
    if strength_override is not None:
        # Caller-computed strength (multi-pass smooth); scale/unified already
        # folded in.
        strength = strength_override
    else:
        strength = bl_brush.strength
        if unified is not None and unified.use_unified_strength:
            strength = unified.strength
        strength *= strength_scale

    sc_brush.strength = strength
    sc_brush.radius = world_radius
    if allow_invert:
        sc_brush.invert = bool(invert) ^ bool(bl_brush.direction == 'SUBTRACT')
    else:
        sc_brush.invert = False

    # writeProps() bakes the scalar fields into the kernel's uniform block.
    sc_brush.writeProps()


def apply_brush(bl_brush, unified, sc_brush, *, world_radius, invert, paint=None):
    """Configure a SculptCore Brush from a Blender Brush in one call —
    ``apply_brush_settings`` plus ``apply_dab_state``. Single-dab convenience
    for the headless tests; the stroke operator calls the two parts itself so
    the curve bakes run once per stroke, not per dab."""
    apply_brush_settings(bl_brush, unified, sc_brush, paint=paint)
    apply_dab_state(bl_brush, unified, sc_brush,
                    world_radius=world_radius, invert=invert)
