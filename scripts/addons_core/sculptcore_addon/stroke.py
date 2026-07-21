# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The interactive stroke: a modal operator plus the reusable dab core the
operator and headless tests both drive.

Dabs are spaced along the 2D mouse path (StrokeSpacer, interval = pixel
radius x spacing / 50 — vanilla's percentage-of-diameter semantics, residual
carried across segments) and each spaced point is projected onto the
surface, so stroke density is independent of both the mouse event rate and
the surface deforming under the stroke.
The viewport updates through the external draw provider; the Mesh ID is
written back lazily by the mode's flush callback (memfile encode / save /
render), keeping dabs and stroke release free of the full Mesh write. The
throttled Mesh flush remains only as the no-provider fallback. The dab core
is engine-only (no ``bpy`` state), so ``enter -> N synthetic dabs -> exit``
is scriptable end-to-end.
"""

import bpy

from . import convert, cursor, engine, mapping, stroke_math, symmetry, texture, undo


def _float3(mgr, x, y, z):
    v = mgr.construct("litestl::math::float3")
    v.vec[0] = x
    v.vec[1] = y
    v.vec[2] = z
    return v


class StrokeSpacer:
    """Screen-space dab spacer: buffers the 2D mouse path and emits evenly
    spaced points along a centripetal Catmull-Rom spline (``stroke_math``),
    carrying the walk position across segments so cadence never clusters at a
    joint. Compared to the old linear-polyline walk this smooths jittery input
    (the parity route to Blender's stabilized stroke) without changing dab
    density: the same interval (the operator's vanilla-matched spacing) is
    walked, and each emitted point is still projected onto the surface by the
    operator.

    The first control point emits one raw dab immediately; a 1-segment lookahead
    holds each interior segment until its right neighbor arrives (the
    centripetal tangent needs the next point), so spline dabs lag input by one
    move; the trailing segment is flushed with a right-clamp on release
    (``flush``). A non-positive interval bypasses the spline and emits every
    input point. Emitted points are ``(x, y)`` tuples."""

    def __init__(self):
        self.points = []
        self.walk_carry = 0.0

    def add(self, p, spacing):
        """Dab points for the newly arrived control point ``p`` (an ``(x, y)``
        pair). The first call emits ``p`` itself (raw); later calls emit the
        spaced points of the segment whose right neighbor ``p`` just
        completed."""
        p = (float(p[0]), float(p[1]))
        self.points.append(p)
        n = len(self.points)
        if n == 1 or spacing <= 0.0:
            return [p]
        if n < 3:
            # The first segment still lacks the right neighbor its tangent needs.
            return []
        # points[n-3] -> points[n-2], with right neighbor points[n-1] = p.
        return self._walk_segment(n - 3, right_clamp=False, spacing=spacing)

    def flush(self, spacing):
        """Emit the trailing segment (the last two points) with a right-clamp,
        on stroke release. No-op for a single-point stroke or non-positive
        spacing."""
        n = len(self.points)
        if n < 2 or spacing <= 0.0:
            return []
        return self._walk_segment(n - 2, right_clamp=True, spacing=spacing)

    def _walk_segment(self, i, right_clamp, spacing):
        """Arc-length-walk the Catmull-Rom segment ``points[i] -> points[i+1]``,
        clamping the outer control points at the path ends."""
        pts = self.points
        p1, p2 = pts[i], pts[i + 1]
        p0 = pts[i - 1] if i >= 1 else p1
        p3 = p2 if right_clamp else pts[i + 2]
        bez = stroke_math.cr_to_bezier(p0, p1, p2, p3)
        emitted, self.walk_carry = stroke_math.arc_length_walk(bez, spacing, self.walk_carry)
        return emitted


def _ensure_brush(session):
    """The session's reusable engine Brush (built once)."""
    if session.brush_obj is None:
        mgr = engine.manager()
        session.brush_obj = mgr.construct("sculptcore::brush::Brush")
    return session.brush_obj


def _ensure_executor(session):
    """The session's CommandExecutor, bound to its tree + brush, with a
    per-session MeshLog wired in so each stroke records an undo step."""
    if session.executor is None:
        mgr = engine.manager()
        ctor = mgr.get_struct("sculptcore::brush::CommandExecutor").find_constructor("main")
        session.executor = mgr.construct_with(ctor, session.tree(), _ensure_brush(session))
        session.meshlog = mgr.construct("sculptcore::meshlog::MeshLog")
        session.executor.meshLog = session.meshlog
        # Csr (ring-1 CSR adjacency) for the smooth family's for_neighbor source,
        # matching the engine's other sculpt consumer. Bit-identical to the
        # LiveDisk default on our freshly built meshes and marginally faster (Q1a
        # A/B); a dyntopo step overrides it back to LiveDisk internally.
        session.executor.setNeighborMode(1)
    return session.executor


def stroke_begin(session, *, has_dyntopo=False, accumulate=True):
    executor = _ensure_executor(session)
    executor.beginStep(has_dyntopo)
    session.dyntopo_active = has_dyntopo
    # A nonzero, per-stroke generation is required for grab-class kernels
    # (they orig-stamp against it); harmless for the rest.
    session.stroke_gen += 1
    executor.setStrokeGen(session.stroke_gen)
    # Vanilla's per-brush "Accumulate": with it off, the engine measures each
    # accumulable command from a stroke-start snapshot (nonAccum mode) so
    # repeated passes within one stroke don't build up.
    executor.setNonAccum(not accumulate)


def smooth_iteration_strengths(strength):
    """Vanilla smooth-brush semantics (#iteration_strengths): the strength
    (clamped to 1) maps to `int(strength * 4)` full-strength relaxation
    passes per dab plus one remainder pass, so higher strength iterates more
    instead of overshooting a single pass."""
    clamped = min(max(strength, 0.0), 1.0)
    count = int(clamped * 4)
    last = 4.0 * (clamped - count / 4.0)
    passes = [1.0] * count
    if last > 1e-4:
        passes.append(last)
    return passes


# Vanilla dyntopo detail constants (sculpt_dyntopo.hh).
DYNTOPO_EDGE_MIN_FACTOR = 0.4       # EDGE_LENGTH_MIN_FACTOR
_DYNTOPO_RELATIVE_SCALE = 0.4       # RELATIVE_SCALE_FACTOR

# Blender detail_refine_method -> engine DynTopoMode value.
_DYNTOPO_REFINE_MODES = {'SUBDIVIDE': 0, 'COLLAPSE': 1, 'SUBDIVIDE_COLLAPSE': 2}


def dyntopo_max_edge(sculpt, ob, world_radius, pixel_radius, pixel_size):
    """Object-space max edge length from Blender's dyntopo detail settings
    (ports #constant_to_detail_size / #brush_to_detail_size /
    #relative_to_detail_size). RELATIVE and BRUSH scale with the dab's world
    radius; CONSTANT/MANUAL are view-independent."""
    method = sculpt.detail_type_method
    if method in {'CONSTANT', 'MANUAL'}:
        # mat4_to_scale equivalent: the mean axis scale of the object matrix.
        scale_vector = ob.matrix_world.to_scale()
        scale = (abs(scale_vector[0]) + abs(scale_vector[1]) + abs(scale_vector[2])) / 3.0
        return 1.0 / (max(sculpt.constant_detail_resolution, 0.0001) * max(scale, 1e-8))
    if method == 'BRUSH':
        return world_radius * sculpt.detail_percent / 100.0
    return ((world_radius / max(pixel_radius, 1.0))
            * (sculpt.detail_size * pixel_size) / _DYNTOPO_RELATIVE_SCALE)


def configure_dyntopo_params(params, scene, refine_method):
    """Apply the per-scene remesher tuning (props.py) plus the refine method
    onto a DynTopoParams. An unset refine method (older files carry DNA
    flags 0, which RNA reads as '') falls back to the default Both."""
    params.mode = _DYNTOPO_REFINE_MODES.get(refine_method, 2)
    params.do_flips = scene.sculptcore_dyntopo_flips
    params.do_smooth = scene.sculptcore_dyntopo_smooth
    params.smooth_lambda = scene.sculptcore_dyntopo_smooth_lambda
    params.reproject_uvs = scene.sculptcore_reproject_uvs
    params.max_rounds = scene.sculptcore_dyntopo_max_rounds
    params.max_splits = scene.sculptcore_dyntopo_split_budget
    params.max_collapses = scene.sculptcore_dyntopo_collapse_budget


def build_dyntopo_params(session, l_max, l_min):
    """Reusable DynTopoParams (edge-length bounds in object space)."""
    mgr = engine.manager()
    if session.dtparams is None:
        session.dtparams = mgr.construct("sculptcore::dyntopo::DynTopoParams")
    p = session.dtparams
    p.l_max = l_max
    p.l_min = min(l_min, l_max * 0.5)
    return p


def dyntopo_due(stroke_s, last_dyntopo_s, spacing):
    """Whether a remesh is due at stroke arc-length ``stroke_s``: true once the
    stroke has travelled ``spacing`` since the last remesh at ``last_dyntopo_s``.
    A ``spacing`` of 0 is due on every dab (the every-dab baseline)."""
    return stroke_s - last_dyntopo_s >= spacing


def apply_dyntopo_dab(session, program, center, normal, radius, params, seed):
    """One program dab through ``applyDab``. With ``params`` set it also runs the
    dyntopo remesh pass; with ``params`` None it is a plain deforming dab (no
    remesh) — the reference's ``params ?? 0`` path for the off-cadence dabs
    between remeshes."""
    mgr = engine.manager()
    executor = _ensure_executor(session)
    center_v = _float3(mgr, *center)
    normal_v = _float3(mgr, *normal)
    try:
        executor.setGrabAccumAdd(False)
        executor.applyDab(program, center_v, normal_v, radius, params, seed)
    finally:
        center_v.dispose()
        normal_v.dispose()


def apply_dab(session, brush_type, center, normal, radius):
    """Run one dab at an object-space center/normal. `center`/`normal` are
    3-tuples; `brush_type` is the SculptBrushes enum value. Returns the
    number of spatial nodes the dab touched (0 = brush missed the surface)."""
    import sculptcore

    mgr = engine.manager()
    executor = _ensure_executor(session)
    tree = session.tree()

    center_v = _float3(mgr, *center)
    normal_v = _float3(mgr, *normal)
    nodes = mgr.construct("litestl::util::Vector<sculptcore::spatial::SpatialNode*,4>")
    try:
        if not tree.filterNodes(center_v, radius, nodes):
            return 0
        # New logical dab primary image: grab-class kernels re-base from the
        # stroke-start position each dab instead of accumulating (mirrors the
        # native harness). No-op for non-grab kernels.
        executor.setGrabAccumAdd(False)
        executor.execBrush(session.mesh(), brush_type, nodes, center_v, normal_v)
        return len(sculptcore.BoundVector(mgr, nodes.ptr, nodes.bind_type))
    finally:
        for obj in (nodes, center_v, normal_v):
            obj.dispose()


def apply_grab_dab(session, brush_type, anchor, cursor, normal, radius, accum_add=False):
    """Grab-class dab: deform the fixed region under `anchor` by the
    cumulative cursor delta (`brush.grabTo`/`grabFrom`). The dab centers on the
    anchor and the node filter widens by the drag distance so the anchored
    region stays covered as the cursor moves away.

    `accum_add` marks a later symmetry image of the same logical dab: False
    (the primary) begins a new logical dab and re-bases every touched vert from
    its stroke-start position; True adds this image's delta on top, so a vertex
    on a symmetry plane touched by two images sums both reflections."""
    mgr = engine.manager()
    executor = _ensure_executor(session)
    brush = session.brush_obj
    tree = session.tree()

    gf = brush.grabFrom.vec
    gt = brush.grabTo.vec
    drag = 0.0
    for i in range(3):
        gf[i] = anchor[i]
        gt[i] = cursor[i]
        drag += (cursor[i] - anchor[i]) ** 2
    drag = drag ** 0.5

    anchor_v = _float3(mgr, *anchor)
    normal_v = _float3(mgr, *normal)
    nodes = mgr.construct("litestl::util::Vector<sculptcore::spatial::SpatialNode*,4>")
    try:
        if not tree.filterNodes(anchor_v, radius + drag, nodes):
            return 0
        executor.setGrabAccumAdd(accum_add)
        executor.execBrush(session.mesh(), brush_type, nodes, anchor_v, normal_v)
        import sculptcore
        return len(sculptcore.BoundVector(mgr, nodes.ptr, nodes.bind_type))
    finally:
        for obj in (nodes, anchor_v, normal_v):
            obj.dispose()


def build_program(session, main_kernel, smooth_factor=0.0):
    """Build a program `[main]`, or `[main, BSMOOTH]` when `smooth_factor > 0`
    (autosmooth). Owned by the session and reused across dabs; also the dab
    unit for the dyntopo path (applyDab takes a program)."""
    mgr = engine.manager()
    if session.program is None:
        session.program = mgr.construct("sculptcore::brush::BrushProgram")
    prog = session.program
    prog.clear()
    prog.addCommand(main_kernel)
    if smooth_factor > 0.0:
        smooth = int(mgr.get("sculptcore::brush::SculptBrushes").items["BSMOOTH"])
        idx = prog.addCommand(smooth)
        # Pin the chained smooth to non-inverted: it shares the Brush with the
        # main command, so a Ctrl-inverted dab would otherwise negate the
        # smooth strength too — an anti-Laplacian that explodes the mesh.
        prog.setCommandInvert(idx, False)
        # BrushProp::Strength == 0. The runtime can't marshal a string arg into
        # a util::string method param, so the smooth strength is overridden by
        # propId, not by name (setCommandFloatByName).
        prog.setCommandFloat(idx, 0, smooth_factor)
    return prog


def apply_dab_program(session, program, center, normal, radius):
    """Run a BrushProgram (e.g. [main, BSMOOTH]) for one dab."""
    import sculptcore

    mgr = engine.manager()
    executor = _ensure_executor(session)
    tree = session.tree()
    center_v = _float3(mgr, *center)
    normal_v = _float3(mgr, *normal)
    nodes = mgr.construct("litestl::util::Vector<sculptcore::spatial::SpatialNode*,4>")
    try:
        if not tree.filterNodes(center_v, radius, nodes):
            return 0
        executor.setGrabAccumAdd(False)
        executor.execProgram(program, nodes, center_v, normal_v)
        return len(sculptcore.BoundVector(mgr, nodes.ptr, nodes.bind_type))
    finally:
        for obj in (nodes, center_v, normal_v):
            obj.dispose()


def stroke_end(session):
    executor = _ensure_executor(session)
    if session.dyntopo_active:
        executor.endDynTopoStroke()
    executor.endStep()
    # endStep() advanced the meshlog's applied-step count; mirror it (a stroke
    # begun after an undo truncates the redo branch, so +1 is always correct).
    session.meshlog_cursor += 1
    session.mesh().recalc_normals()


def raycast(session, origin, direction):
    """Cast a ray (object space) against the engine tree; returns a
    (position, normal, face_index) tuple on hit, else None."""
    mgr = engine.manager()
    tree = session.tree()
    orig_v = _float3(mgr, *origin)
    dir_v = _float3(mgr, *direction)
    hit = mgr.construct("sculptcore::spatial::CastRayIsect")
    try:
        if not tree.castRay(orig_v, dir_v, hit):
            return None
        p = tuple(hit.p.vec)
        n = tuple(hit.normal.vec)
        return (p, n, hit.faceIndex)
    finally:
        for obj in (orig_v, dir_v, hit):
            obj.dispose()


class SCULPTCORE_OT_brush_stroke(bpy.types.Operator):
    bl_idname = "sculptcore.brush_stroke"
    bl_label = "SculptCore Stroke"
    # No 'UNDO': the stroke pushes its own CUSTOM_MODE step (delta undo) at end
    # via undo.push, instead of a full memfile snapshot. The Mesh ID stays
    # authoritative through the mode's flush for save/render.
    bl_options = set()

    # SKIP_SAVE (like vanilla paint_stroke_operator_properties): without it a
    # plain-LMB stroke reuses the last-used value, so one Ctrl/Shift stroke
    # would latch INVERT/SMOOTH permanently.
    mode: bpy.props.EnumProperty(
        name="Stroke Mode",
        items=(
            ('NORMAL', "Regular", "Apply brush normally"),
            ('INVERT', "Invert", "Invert action of brush for duration of stroke"),
            ('SMOOTH', "Smooth", "Switch brush to smooth mode for duration of stroke"),
            ('MASK', "Mask", "Switch brush to the mask brush for duration of stroke"),
        ),
        default='NORMAL',
        options={'SKIP_SAVE'},
    )

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return (
            ob is not None
            and ob.mode == 'CUSTOM'
            and ob.custom_mode == "sculptcore.sculpt"
            and ob.name in engine.sessions
        )

    def invoke(self, context, event):
        ob = context.active_object
        # A foreign memfile undo may have changed the mesh under us (custom-undo
        # modes skip the generic refresh); rebuild the session before sculpting
        # if the engine no longer matches the Mesh.
        convert.resync_if_diverged(ob)
        self.session = engine.sessions[ob.name]
        self.brush = context.tool_settings.sculpt.brush
        mgr = engine.manager()
        if self.mode in {'SMOOTH', 'MASK'}:
            # Shift-stroke smooths, Alt-stroke masks — both with the active
            # brush's radius/strength (vanilla brush_toggle semantics).
            kernel_name = "BSMOOTH" if self.mode == 'SMOOTH' else 'MASK'
            self.kernel = (int(mgr.get("sculptcore::brush::SculptBrushes").items[kernel_name])
                           if self.brush else None)
        else:
            self.kernel = mapping.kernel_enum(mgr, self.brush) if self.brush else None
        if self.kernel is None:
            self.report({'WARNING'}, "SculptCore: brush type has no kernel")
            return {'CANCELLED'}

        self._last_flush = 0.0
        self._dab_count = 0
        # A kernel toggle (smooth/mask) replaces the brush's own kernel, so
        # brush-type-derived behavior (grab anchoring, face-set group
        # assignment, autosmooth chaining) is bypassed for the stroke.
        kernel_toggle = self.mode in {'SMOOTH', 'MASK'}
        self._grab_class = not kernel_toggle and mapping.is_grab_class(self.brush)
        # Smoothing strokes (Shift-toggle or the Smooth brush itself) iterate
        # per dab by strength, vanilla-style (see smooth_iteration_strengths).
        self._smooth_stroke = (
            self.mode == 'SMOOTH'
            or (not kernel_toggle and self.brush.sculpt_brush_type == 'SMOOTH'))
        # Anchored / Drag-Dot stroke methods drive the engine preview-dab API
        # (one live, non-compounding dab per input) instead of the spacer. DOTS/
        # SPACE (and, for now, AIRBRUSH/LINE/CURVE) use the spacer path; grab
        # anchors via its own kernel, so it keeps the grab path.
        self._stroke_method = self.brush.stroke_method
        self._preview_method = (not self._grab_class
                                and self._stroke_method in {'ANCHORED', 'DRAG_DOT'})
        # Tablet pressure (M4): Blender maps pressure to strength/size through
        # the Brush.curve_strength / curve_size response curves, baked once here
        # (never per dab) into Python-side LUTs. The smooth brush folds pressure
        # in through them (it re-runs the kernel per relaxation pass, whose
        # per-node loadProps would re-consume an engine device sample); the
        # normal path additionally drives the engine device dynamics (each dab
        # refills the sample; mouse reports 1.0, a no-op). The cursor overlay
        # scales its radius by the size LUT. Grab-class strokes get no pressure.
        sc_brush = _ensure_brush(self.session)
        use_strength = not self._grab_class and self.brush.use_pressure_strength
        use_size = not self._grab_class and self.brush.use_pressure_size
        self._use_pressure = use_strength or use_size
        self._pressure_strength_lut = (
            mapping.sample_pressure_curve(self.brush.curve_strength) if use_strength else None)
        self._pressure_size_lut = (
            mapping.sample_pressure_curve(self.brush.curve_size) if use_size else None)
        if self._smooth_stroke:
            sc_brush.clearPropDynamics(mapping.PROP_STRENGTH)
            sc_brush.clearPropDynamics(mapping.PROP_RADIUS)
        else:
            mapping.apply_pressure_dynamics(
                self.brush, sc_brush, use_strength=use_strength, use_size=use_size)
        # Brush texture (Phase 2): bind or clear per stroke; view-pinned
        # mappings also need the current perspective matrix.
        texture.apply_texture(self.brush, sc_brush)
        if texture.needs_render_matrix(self.brush):
            texture.apply_render_matrix(context, _ensure_executor(self.session))
        # Stroke-constant brush settings, including the falloff/cavity curve
        # bakes (256 engine calls each): once per stroke. The dab paths write
        # only the radius/invert state on top (mapping.apply_dab_state).
        paint = context.tool_settings.sculpt
        mapping.apply_brush_settings(
            self.brush, paint.unified_paint_settings, sc_brush, paint=paint)
        # UV slide-reprojection (scene toggle): the executor re-anchors moved
        # verts' UVs for the smooth-family kernels. Any stroke that may run
        # one (smooth brush, Shift-smooth, autosmooth chain) diverges the
        # engine UVs from the Mesh, so the flush must write them back.
        sc_brush.reproject_uvs = context.scene.sculptcore_reproject_uvs
        if sc_brush.reproject_uvs:
            self.session.uv_dirty = True
        # "Adjust Strength for Spacing": constant for the stroke, folded into
        # every dab's strength write.
        self._overlap = mapping.overlap_attenuation(self.brush)
        self._anchor = None
        self._anchor_normal = None
        # Dab spacing along the stroke path (engine StrokeSpacer semantics:
        # interval = world radius x spacing fraction). Grab-class ignores it.
        self._spacer = StrokeSpacer()
        # Trailing-flush state, refreshed on every move (see _dab_at); defaults
        # cover a commit with no intervening move.
        self._last_invert = False
        self._last_pressure = 1.0
        self._last_spacing = 0.0
        # Dyntopo cadence bookkeeping: accumulated stroke arc length and the
        # arc length of the last remesh. -inf so the first dab always remeshes.
        self._stroke_s = 0.0
        self._last_dyntopo_s = float("-inf")
        self._dyntopo_spacing = 0.0
        # Plane-mirror symmetry: one sign vector per reflection (empty = off).
        self._mirror_signs = symmetry.mirror_signs(symmetry.axes_from_mesh(ob.data))
        # Face-set brushes paint a fresh group id per stroke.
        if not kernel_toggle and self.brush.sculpt_brush_type in mapping.FACE_SET_TYPES:
            brush = _ensure_brush(self.session)
            brush.activeGroup = int(self.session.mesh().maxFaceGroup()) + 1
        # Dyntopo (scene toggle) and autosmooth both run through a program;
        # neither applies to grab-class nor to a Shift-smooth stroke.
        # Autosmooth also skips the smooth brush itself.
        scene = context.scene
        smooth_factor = 0.0
        if (not self._grab_class and not kernel_toggle
                and self.brush.sculpt_brush_type != 'SMOOTH'
                and self.brush.auto_smooth_factor > 0.0):
            smooth_factor = self.brush.auto_smooth_factor

        self._dyntopo = None
        self._program = None
        self._detail_factor = None
        if (not self._grab_class and not kernel_toggle
                and getattr(scene, "sculptcore_dyntopo", False)):
            self._program = build_program(self.session, self.kernel, smooth_factor)
            # Detail size from Blender's dyntopo settings (see
            # dyntopo_max_edge). CONSTANT/MANUAL fix the edge length for the
            # stroke; RELATIVE/BRUSH reduce to `factor * world_radius`,
            # re-applied per remesh dab (the radius is depth-dependent).
            sculpt_settings = context.tool_settings.sculpt
            unified = sculpt_settings.unified_paint_settings
            pixel_radius = unified.size if unified.use_unified_size else self.brush.size
            if sculpt_settings.detail_type_method in {'CONSTANT', 'MANUAL'}:
                l_max = dyntopo_max_edge(sculpt_settings, ob, 0.0, pixel_radius,
                                         context.preferences.system.pixel_size)
            else:
                # Factor per unit world radius (formulas are linear in it).
                self._detail_factor = dyntopo_max_edge(
                    sculpt_settings, ob, 1.0, pixel_radius,
                    context.preferences.system.pixel_size)
                l_max = self._detail_factor  # placeholder; set per remesh dab
            self._dyntopo = build_dyntopo_params(
                self.session, l_max, l_max * DYNTOPO_EDGE_MIN_FACTOR)
            configure_dyntopo_params(self._dyntopo, scene,
                                     sculpt_settings.detail_refine_method)
            # Remesh cadence in the same (pixel) units stroke_s accumulates: a
            # fraction of the brush diameter of stroke travel per remesh pass.
            frac = max(getattr(scene, "sculptcore_dyntopo_spacing", 0.5), 0.0)
            self._dyntopo_spacing = frac * 2.0 * pixel_radius
        elif smooth_factor > 0.0:
            self._program = build_program(self.session, self.kernel, smooth_factor)

        # Anchored refuses a stroke that starts off the surface — checked before
        # opening the undo step, so a refusal leaves no empty step behind.
        if self._preview_method and self._stroke_method == 'ANCHORED':
            a_origin, a_dir = _ray_origin_dir(
                context, (event.mouse_region_x, event.mouse_region_y))
            a_hit = raycast(self.session, a_origin, a_dir)
            if a_hit is None:
                self.report({'WARNING'},
                            "SculptCore: anchored stroke must start on the surface")
                return {'CANCELLED'}
            self._anchor = a_hit[0]
            self._anchor_normal = a_hit[1]
            self._anchor_screen = (event.mouse_region_x, event.mouse_region_y)
            self._anchor_radius = _world_radius(context, self.brush, a_hit[0])

        # Kernel toggles (smooth/mask) accumulate inherently, like vanilla,
        # and non-accumulate only exists where vanilla shows the option
        # (has_accumulate): for kernels without the concept — smooth is a
        # relaxation, not a displacement — the engine's snapshot re-basing
        # (nonAccum) is nonsense and blows the geometry up.
        accumulate = (self.mode in {'SMOOTH', 'MASK'}
                      or self._grab_class
                      or not self.brush.sculpt_capabilities.has_accumulate
                      or self.brush.use_accumulate)
        stroke_begin(self.session, has_dyntopo=self._dyntopo is not None,
                     accumulate=accumulate)
        context.window_manager.modal_handler_add(self)
        # First dab at the invoke location.
        self._publish_cursor_pressure(event.pressure)
        if self._preview_method:
            self._dab_preview(context, event)
        else:
            self._dab_at(context, event)
        return {'RUNNING_MODAL'}

    def _dab_at(self, context, event):
        paint = context.tool_settings.sculpt
        unified = paint.unified_paint_settings
        # The keymap sets INVERT for Ctrl-LMB; live Ctrl also inverts so the
        # direction can be toggled mid-stroke.
        invert = event.ctrl or self.mode == 'INVERT'

        if self._grab_class:
            hit = _ray_from_event(context, event, self.session)
            if hit is None:
                return
            position, normal, _face = hit
            world_radius = _world_radius(context, self.brush, position)
            mapping.apply_dab_state(self.brush, unified, self.session.brush_obj,
                                    world_radius=world_radius, invert=invert, strength_scale=self._overlap)
            if self._anchor is None:
                # Anchor the region at the stroke-start surface point.
                self._anchor = position
                self._anchor_normal = normal
                self._anchor_radius = world_radius
            # Project the current mouse onto the plane through the anchor to
            # get the drag target in object space.
            cursor = _cursor_on_anchor_plane(context, event, self._anchor)
            apply_grab_dab(self.session, self.kernel, self._anchor, cursor,
                           self._anchor_normal, self._anchor_radius)
            # Symmetry: reflect the anchor, cursor and normal directly (no
            # re-raycast for grab — the resolved plane point is used as-is).
            for sign in self._mirror_signs:
                apply_grab_dab(
                    self.session, self.kernel,
                    symmetry.reflect(self._anchor, sign),
                    symmetry.reflect(cursor, sign),
                    symmetry.reflect(self._anchor_normal, sign),
                    self._anchor_radius, accum_add=True)
        else:
            pixel_size = unified.size if unified.use_unified_size else self.brush.size
            # Vanilla spacing is a percentage of the brush *diameter*:
            # radius * spacing / 50 (#paint_space_stroke_spacing).
            step = max(self.brush.spacing, 1) / 50.0 * pixel_size
            coord = (event.mouse_region_x, event.mouse_region_y)
            # Remember the last-move state so the trailing spline segment can be
            # flushed on release (the release event carries no spline context).
            self._last_invert = invert
            self._last_pressure = event.pressure
            self._last_spacing = step
            for point in self._spacer.add(coord, step):
                self._apply_spaced_dab(context, point, invert, event.pressure)

        self._mid_redraw(context)

    def _mid_redraw(self, context):
        """Throttled mid-stroke refresh: the draw provider needs only its GPU
        buffers refreshed; the Mesh write-back is deferred to the mode's flush
        callback. The full flush remains only for the no-provider fallback,
        where the viewport draws the Mesh itself."""
        import time
        now = time.monotonic()
        if now - self._last_flush > 1.0 / 30.0:
            if self.session.draw_key:
                convert.draw_refresh(context.active_object)
            else:
                convert.flush(context.active_object)
            self._last_flush = now
        context.area.tag_redraw()

    def _apply_one_image(self, position, normal, world_radius, due):
        """Apply one dab image (primary or a symmetry mirror) through the active
        path — plain, autosmooth-program, or dyntopo. Each image gets a unique
        monotonic seed (dyntopo independent-set selection)."""
        self._dab_count += 1
        seed = self._dab_count
        if self._dyntopo is not None:
            apply_dyntopo_dab(self.session, self._program, position, normal,
                              world_radius, self._dyntopo if due else None, seed)
        elif self._program is not None:
            apply_dab_program(self.session, self._program, position, normal,
                              world_radius)
        else:
            apply_dab(self.session, self.kernel, position, normal, world_radius)

    def _apply_spaced_dab(self, context, point, invert, pressure):
        """Project one spacer-emitted 2D point onto the surface and apply the
        primary dab plus one reflected dab per symmetry mirror."""
        origin, direction = _ray_origin_dir(context, point)
        hit = raycast(self.session, origin, direction)
        if hit is None:
            return
        position, normal, _face = hit
        world_radius = _world_radius(context, self.brush, position)
        unified = context.tool_settings.sculpt.unified_paint_settings
        # Advance the stroke arc length and decide the dyntopo cadence once per
        # logical dab, so every mirror image remeshes on the same samples
        # (a per-image decision would let the primary starve the mirrors).
        self._stroke_s += self._last_spacing
        due = (self._dyntopo is not None
               and dyntopo_due(self._stroke_s, self._last_dyntopo_s, self._dyntopo_spacing))
        if due:
            self._last_dyntopo_s = self._stroke_s
            if self._detail_factor is not None:
                # RELATIVE/BRUSH detail scales with the (depth-dependent)
                # world radius; refresh the bounds for this remesh pass.
                l_max = self._detail_factor * world_radius
                build_dyntopo_params(self.session, l_max,
                                     l_max * DYNTOPO_EDGE_MIN_FACTOR)

        if self._smooth_stroke:
            # Multi-pass smooth (vanilla semantics): strength maps to N
            # relaxation passes at explicit per-pass strengths. Pressure and the
            # overlap factor fold into the base Python-side (smooth registers no
            # engine dynamics — see invoke); the pressure factor comes from the
            # baked curve_strength / curve_size response LUTs.
            strength = self.brush.strength
            if unified.use_unified_strength:
                strength = unified.strength
            if self._pressure_strength_lut is not None:
                strength *= mapping.eval_pressure_lut(self._pressure_strength_lut, pressure)
            if self._pressure_size_lut is not None:
                world_radius *= mapping.eval_pressure_lut(self._pressure_size_lut, pressure)
            for pass_strength in smooth_iteration_strengths(strength * self._overlap):
                # Smoothing has no inverse (see apply_dab_state): ignore Ctrl
                # and the brush direction for the smooth passes.
                mapping.apply_dab_state(self.brush, unified, self.session.brush_obj,
                                        world_radius=world_radius, invert=False,
                                        strength_override=pass_strength,
                                        allow_invert=False)
                self._apply_one_image(position, normal, world_radius, due)
                for sign in self._mirror_signs:
                    self._apply_one_image(symmetry.reflect(position, sign),
                                          symmetry.reflect(normal, sign),
                                          world_radius, due)
                due = False  # remesh at most once per logical dab
            return

        mapping.apply_dab_state(self.brush, unified, self.session.brush_obj,
                                world_radius=world_radius, invert=invert, strength_scale=self._overlap)
        if self._use_pressure:
            # The executor consumes the device samples in loadProps; refill per
            # dab (engine bridge convention).
            sc = self.session.brush_obj
            sc.clearDeviceInputs()
            sc.pushDeviceInput(mapping.DEVICE_PRESSURE, pressure)
        self._apply_one_image(position, normal, world_radius, due)
        # Symmetry mirror images: reflect the resolved primary center and normal
        # directly (mirror the operation, as vanilla sculpt does), reusing the
        # primary's world radius. Re-raycasting the mirrored view ray instead —
        # as the reference app does — is not reflection-equivariant here: the
        # engine's castRay reconstructs the hit position imprecisely (off the
        # ray by ~tessellation scale), which leaves visible asymmetry. Reflecting
        # the resolved hit is exact.
        for sign in self._mirror_signs:
            self._apply_one_image(symmetry.reflect(position, sign),
                                  symmetry.reflect(normal, sign),
                                  world_radius, due)

    def _finish(self, context, status):
        ob = context.active_object
        cursor.set_size_scale(1.0)
        # On commit, flush the trailing spline segment held back by the
        # 1-segment lookahead (right-clamped); cancel drops it.
        if status == 'FINISHED' and not self._grab_class:
            for point in self._spacer.flush(self._last_spacing):
                self._apply_spaced_dab(context, point, self._last_invert,
                                       self._last_pressure)
        stroke_end(self.session)
        # Deferred write-back: with the draw provider active the viewport only
        # needs its GPU buffers; the Mesh ID syncs on demand through the
        # mode's flush callback (memfile encode / save / render). This keeps
        # stroke release free of the full Mesh (or MDISPS bake) write.
        if self.session.draw_key:
            convert.draw_refresh(ob)
        else:
            convert.flush(ob)
        # The stroke mutated geometry (dabs applied before release/cancel), so
        # push its delta-undo step regardless of finish vs cancel.
        undo.push(context, ob, self.session)
        context.area.tag_redraw()
        return {status}

    def _preview_apply_image(self, center, normal, world_radius, extend):
        """Snapshot one dab image's region into the open preview session
        (``begin`` for the primary, ``extend`` for each mirror), then deform it.
        Snapshotting before the deform lets the whole group roll back together.
        No dyntopo in the preview path (anchored/drag-dot deform only)."""
        mgr = engine.manager()
        executor = _ensure_executor(self.session)
        center_v = _float3(mgr, *center)
        try:
            if extend:
                executor.extendPreviewDab(center_v, world_radius)
            else:
                executor.beginPreviewDab(center_v, world_radius)
        finally:
            center_v.dispose()
        if self._program is not None:
            apply_dab_program(self.session, self._program, center, normal, world_radius)
        else:
            apply_dab(self.session, self.kernel, center, normal, world_radius)

    def _dab_preview(self, context, event):
        """Anchored / Drag-Dot: one live dab (plus mirrors) inside a preview
        bracket so successive inputs never compound. Resolve the new dab first,
        then roll back the previous provisional group and apply the new one."""
        invert = event.ctrl or self.mode == 'INVERT'
        executor = _ensure_executor(self.session)
        coord = (event.mouse_region_x, event.mouse_region_y)

        if self._stroke_method == 'ANCHORED':
            # Dab pinned at the anchor; radius grows with screen-space drag.
            center = self._anchor
            normal = self._anchor_normal
            import mathutils
            drag_px = (mathutils.Vector(coord)
                       - mathutils.Vector(self._anchor_screen)).length
            # Radius is the unprojected drag length (0 at the anchor, growing as
            # the cursor pulls away); check `is None` so a genuine 0 is kept.
            world_radius = _pixel_to_world_length(context, center, drag_px)
            if world_radius is None:
                world_radius = self._anchor_radius
        else:  # DRAG_DOT: one dab at the live cursor.
            origin, direction = _ray_origin_dir(context, coord)
            hit = raycast(self.session, origin, direction)
            if hit is None:
                # Cursor off the surface: keep the previous provisional dab.
                return
            center, normal, _ = hit
            world_radius = _world_radius(context, self.brush, center)

        # Roll back the previous provisional group only now that a new dab is
        # resolved (a drag-dot miss above leaves the last dab intact).
        if executor.previewActive():
            executor.rollbackPreviewDab()
        unified = context.tool_settings.sculpt.unified_paint_settings
        if self._smooth_stroke:
            # Smooth registers no engine dynamics (see invoke): fold pressure
            # into strength / radius Python-side through the baked LUTs.
            strength = self.brush.strength
            if unified.use_unified_strength:
                strength = unified.strength
            strength *= self._overlap
            if self._pressure_strength_lut is not None:
                strength *= mapping.eval_pressure_lut(self._pressure_strength_lut, event.pressure)
            if self._pressure_size_lut is not None:
                world_radius *= mapping.eval_pressure_lut(self._pressure_size_lut, event.pressure)
            mapping.apply_dab_state(self.brush, unified, self.session.brush_obj,
                                    world_radius=world_radius, invert=invert,
                                    strength_override=strength, allow_invert=False)
        else:
            mapping.apply_dab_state(self.brush, unified, self.session.brush_obj,
                                    world_radius=world_radius, invert=invert,
                                    strength_scale=self._overlap, allow_invert=True)
            if self._use_pressure:
                sc = self.session.brush_obj
                sc.clearDeviceInputs()
                sc.pushDeviceInput(mapping.DEVICE_PRESSURE, event.pressure)
        self._preview_apply_image(center, normal, world_radius, extend=False)
        # Mirror images share the one preview bracket, so one rollback reverts
        # the whole group.
        for sign in self._mirror_signs:
            self._preview_apply_image(symmetry.reflect(center, sign),
                                      symmetry.reflect(normal, sign),
                                      world_radius, extend=True)
        self._mid_redraw(context)

    def _finish_preview(self, context, commit):
        """End an anchored / drag-dot stroke: commit keeps the one live dab and
        pushes an undo step; cancel rolls it back and pushes nothing (the mesh
        is left exactly as the stroke began)."""
        ob = context.active_object
        cursor.set_size_scale(1.0)
        executor = _ensure_executor(self.session)
        if executor.previewActive():
            if commit:
                executor.commitPreviewDab()
            else:
                executor.rollbackPreviewDab()
        stroke_end(self.session)
        if self.session.draw_key:
            convert.draw_refresh(ob)
        else:
            convert.flush(ob)
        if commit:
            undo.push(context, ob, self.session)
        context.area.tag_redraw()
        return {'FINISHED' if commit else 'CANCELLED'}

    def _publish_cursor_pressure(self, pressure):
        """Scale the viewport cursor circle by the current size-pressure factor
        so it tracks the pen like the deformation does (1.0 when size pressure
        is off)."""
        scale = 1.0
        if self._pressure_size_lut is not None:
            scale = mapping.eval_pressure_lut(self._pressure_size_lut, pressure)
        cursor.set_size_scale(scale)

    def modal(self, context, event):
        if event.type == 'MOUSEMOVE':
            self._publish_cursor_pressure(event.pressure)
            if self._preview_method:
                self._dab_preview(context, event)
            else:
                self._dab_at(context, event)
            return {'RUNNING_MODAL'}
        if event.type == 'LEFTMOUSE' and event.value == 'RELEASE':
            if self._preview_method:
                return self._finish_preview(context, commit=True)
            return self._finish(context, 'FINISHED')
        if event.type in {'RIGHTMOUSE', 'ESC'}:
            if self._preview_method:
                return self._finish_preview(context, commit=False)
            return self._finish(context, 'CANCELLED')
        return {'RUNNING_MODAL'}


def _ray_origin_dir(context, coord):
    """Object-space ``(origin, direction)`` of the view ray through a 2D region
    coordinate (both 3-tuples). Split out so symmetry can reflect the ray and
    re-cast it."""
    from bpy_extras import view3d_utils

    region = context.region
    rv3d = context.region_data
    ob = context.active_object

    origin_world = view3d_utils.region_2d_to_origin_3d(region, rv3d, coord)
    direction_world = view3d_utils.region_2d_to_vector_3d(region, rv3d, coord)

    matrix_inv = ob.matrix_world.inverted()
    origin = matrix_inv @ origin_world
    direction = (matrix_inv.to_3x3() @ direction_world).normalized()
    return tuple(origin), tuple(direction)


def _ray_from_coord(context, coord, session):
    """Unproject a 2D region coordinate to an object-space ray and cast it
    against the engine tree."""
    origin, direction = _ray_origin_dir(context, coord)
    return raycast(session, origin, direction)


def _ray_from_event(context, event, session):
    """Unproject the mouse event to an object-space ray and cast it against
    the engine tree."""
    return _ray_from_coord(
        context, (event.mouse_region_x, event.mouse_region_y), session)


def _cursor_on_anchor_plane(context, event, anchor_obj):
    """Object-space point where the mouse ray meets the view-facing plane
    through the anchor — grab's drag target."""
    import mathutils
    from bpy_extras import view3d_utils

    region = context.region
    rv3d = context.region_data
    ob = context.active_object
    coord = (event.mouse_region_x, event.mouse_region_y)

    anchor_world = ob.matrix_world @ mathutils.Vector(anchor_obj)
    loc_world = view3d_utils.region_2d_to_location_3d(region, rv3d, coord, anchor_world)
    return tuple(ob.matrix_world.inverted() @ loc_world)


def _pixel_to_world_length(context, position, pixel_len):
    """Object-space length spanning ``pixel_len`` screen pixels at
    ``position``'s depth (vanilla paint_calc_object_space_radius semantics).
    Returns None when the point projects off-screen."""
    import mathutils
    from bpy_extras import view3d_utils

    region = context.region
    rv3d = context.region_data
    ob = context.active_object

    center_world = ob.matrix_world @ mathutils.Vector(position)
    offset_2d = view3d_utils.location_3d_to_region_2d(region, rv3d, center_world)
    if offset_2d is None:
        return None
    offset_2d = offset_2d.copy()
    offset_2d.x += pixel_len
    ray_origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, offset_2d)
    ray_dir = view3d_utils.region_2d_to_vector_3d(region, rv3d, offset_2d)
    # Object-space distance from the point to the unprojected offset at its depth.
    edge_world = ray_origin + ray_dir * (center_world - ray_origin).length
    matrix_inv = ob.matrix_world.inverted()
    return (matrix_inv @ edge_world - matrix_inv @ center_world).length


def _world_radius(context, brush, position):
    """Object-space dab radius from the brush's pixel size at the dab
    location."""
    unified = context.tool_settings.sculpt.unified_paint_settings
    pixel_size = unified.size if unified.use_unified_size else brush.size
    length = _pixel_to_world_length(context, position, pixel_size)
    return length or (brush.unprojected_size or 1.0)


def register():
    bpy.utils.register_class(SCULPTCORE_OT_brush_stroke)


def unregister():
    bpy.utils.unregister_class(SCULPTCORE_OT_brush_stroke)
