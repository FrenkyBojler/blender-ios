# P10 — UI Parity: SculptCore Mode vs Vanilla Sculpt Mode

Goal: the SculptCore custom mode's UI matches vanilla sculpt mode across the
View3D header, menus, context menu, pies, Tool tab / N-panel, toolbar, asset
shelf, Properties editor, and hotkeys — except where the engine genuinely
lacks a feature, in which case the gap is recorded in an allowlist, not left
silent.

Ground truth is the T1–T4 tooling output
([ui-parity-tooling.md](./ui-parity-tooling.md)); regenerate after every
phase:
- `%TEMP%/ui_poll_matrix.json` — which surfaces exist per mode (T1)
- `%TEMP%/ui_introspect.json` — item-level content of every vanilla sculpt
  panel/menu/pie + header, in both modes (T2)
- `%TEMP%/keymap_dump.json` — chord-level keymap diff (T3)
- `%TEMP%/ui_shot_*.png` — visual side-by-side (T4)

## Starting position (measured 2026-07-19)

- 18 sculpt-specific panels missing; all vanilla Tool-tab panels are
  `bl_context`-gated (".sculpt_mode"/".paint_common") and `context.mode` is
  `'CUSTOM'`, so they can never draw in our mode as-is.
- Header: menubar has only "View" (vanilla: View/Sculpt/Mask/Face Sets),
  none of the symmetry/falloff/texture/stroke/cursor popovers, and the mode
  dropdown shows the raw `sculptcore.sculpt` id although `SculptCoreMode`
  declares `bl_label`/`bl_icon` (C-side selector ignores them — infra bug).
- Keymap: 5 of ~65 vanilla chords bound.
- No toolbar tool list rendering, no brush asset shelf, Properties Tool tab
  nearly empty.
- Working for us already: tool registration under the `'CUSTOM'` context
  mode (tools.py seeds `VIEW3D_PT_tools_active._tools`), tool-header
  draw_settings, `bl_use_sculpt_paint` C-side paint-context routing, shared
  `tool_settings.sculpt` brush + unified settings.

## Strategy

**Reuse vanilla panel code via addon subclasses; write no parallel UI.**
The brush state is the shared `tool_settings.sculpt`, so vanilla draw bodies
are correct for us. What blocks them is mode-string dispatch in the
`UnifiedPaintPanel` helpers (`paint_settings()`, `get_brush_mode()`) and
`bl_context` gating. Addon subclasses inherit `draw` unchanged, override
`poll` (custom mode) and the paint-settings helpers, drop `bl_context`, and
register under the same visible location. Upstream C changes stay minimal:
the mode-label fix (Phase A) and nothing else unless a phase proves it
unavoidable — record any such change in the P2 design doc.

For menus/keymaps, entries resolve to one of three classes (Phase 0 audit):
1. **works-as-is** — operates on shared state or pure WM
   (`wm.radial_control`, `wm.call_menu_pie`, `brush.asset_activate`,
   `brush.scale_size`, unified props);
2. **needs engine op** — vanilla operator touches Mesh/SculptSession
   (`paint.mask_flood_fill`, visibility, face sets, `sculpt.mesh_filter`,
   remesh, `object.subdivision_set`) → addon operator over the engine,
   registered under `sculptcore.*`, menu/keymap entry points at it;
3. **deferred** — engine lacks the feature → omit and record in the
   allowlist (`claudeMemory/plans/ui-parity-allowlist.json`), which T6
   consumes.

## Phase 0 — Operator/capability audit — DONE (2026-07-19)

65 operators referenced by the vanilla surface: 14 class-1, 17 class-2,
34 class-3. Generator: `claudeMemory/scripts/ui_parity_audit.py` (the
classification table lives there); outputs `ui-parity-audit.md` +
`ui-parity-allowlist.json` beside this file. Notable: all visibility ops are
class-3 (engine has no visibility attribute — biggest single capability gap),
mask ops are class-2 over the existing mask column, and
`object.subdivision_set`/dyntopo toggles are easy class-2 wins.

Script over `ui_introspect.json` + `keymap_dump.json`: extract every
operator (with args) and RNA property referenced by the vanilla surface;
classify 1/2/3 against engine capabilities (mapping.py, engine API, p8
scripts as capability evidence). Deliverable: audit table in this file's
companion `ui-parity-audit.md` + first `ui-parity-allowlist.json`. Every
later phase consumes this classification; nothing is hidden ad hoc.

## Phase A — Mode identity & header shell — DONE (2026-07-19)

What landed (regression 12/12 green, verified by T4 header crops + T1):

- **Mode identity**: the header dropdown shows the registered type's
  `bl_label` + `bl_icon`. Two changes: `rna_object_mode.cc` gains
  `STRUCT_PUBLIC_NAMESPACE_INHERIT` so registered modes live in `bpy.types`
  (like Panel subclasses — previously they were absent from `structs_map`),
  and the header resolves `getattr(bpy.types, custom_mode.replace('.','_'))`
  for label/icon.
  Known infra wart (documented, not blocking): registering an
  ObjectModeType subclass invalidates the cached `bpy.types.ObjectModeType`
  wrapper, so `bl_rna_get_subclass_py`/`__subclasses__` on a fresh
  reference miss it — use the `bpy.types` namespace lookup instead.
- **Header popovers, generically**: `VIEW3D_HT_tool_header.
  draw_mode_settings` gains a CUSTOM branch doing
  `popover_group(context=object.custom_mode, category="Tool")` — any custom
  mode's Tool-tab panels become header popovers like built-in modes.
- **Addon panels moved** from the private "SculptCore" N-panel category to
  `bl_category = "Tool"` + `bl_context = "sculptcore.sculpt"`. Key
  mechanism (verified): a custom mode's C context string
  (#CTX_data_mode_string) is its registered idname, and the View3D sidebar
  passes it as `contexts_base[0]`, so bl_context gating works for custom
  modes with no C changes. Header now shows Brush/Automasking/Symmetry/
  Dyntopo popovers + a Mirror X/Y/Z row (tool-header append).
- Deferred into Phase C: the menubar Sculpt/Mask/Face Sets menus (need
  their contents first).

## Phase B — Brush + sculpt panels (sidebar Tool tab) — brush family DONE (2026-07-19)

Landed (`vanilla_panels.py`; regression 12/12; T2 item-diff: all nine panel
pairs IDENTICAL to vanilla):
- Clones of `VIEW3D_PT_tools_brush_select`, `_settings`, `_settings_advanced`,
  `_stroke`, `_stroke_smooth_stroke`, `_falloff`, `_falloff_normal`,
  `_display`, `_texture` under `SCULPTCORE_PT_*` names, category "Tool",
  bl_context "sculptcore.sculpt", mode-gated poll. The texture clone
  overrides draw only to hardcode the sculpt flag (vanilla derives it from
  `context.sculpt_object`).
- **Clone, not subclass** (load-bearing): registering a subclass of a
  registered class makes `bpy.types.<BaseName>` return a bare RNA wrapper
  without the base's Python methods — verified stock-Blender behavior, not
  branch-specific. The factory rebuilds each class from the vanilla class's
  dict on its unregistered mixin bases (none use super()).
- Upstream enablers: `UnifiedPaintPanel.get_brush_mode` maps CUSTOM ->
  'SCULPT' for modes declaring `bl_use_sculpt_paint`; missing None-guard on
  `context.sculpt_object` fixed at the topology-rake branch (the
  neighboring persistent-base branch already had it); the mode's
  WorkSpaceTool sets `bl_options = {'USE_BRUSHES'}` (without it
  get_brush_mode returns None and every brush panel hides).
- `SCULPTCORE_PT_brush` replaced: standard brush UI comes from the clones;
  engine-only uniforms + support warnings live in `SCULPTCORE_PT_brush_engine`
  (child of the settings clone).

Note on T1 accounting: vanilla panels still (correctly) don't poll in
CUSTOM — coverage is via the clones, so T6's allowlist must map vanilla
panel names to their clone equivalents.

Phase B tail decision: the vanilla `sculpt_symmetry` / `sculpt_options`
panels are NOT cloned for now — beyond mirror X/Y/Z their contents
(lock/tiling/feather, symmetrize, gravity, deform-only) are engine gaps, and
a vanilla-shaped panel of dead controls is worse than the current honest
mirror-only panel. Revisit per-property as engine support lands; the gaps
ride the audit allowlist.

## Phase C — Menus, pies, context menu — IN PROGRESS (started 2026-07-19)

Landed (regression 13/13):
- **Context menu**: `VIEW3D_PT_sculpt_context_menu` clone (pure shared-brush
  props, verified by T2) + RMB/APP `wm.call_panel` chords. The clone
  factory now takes a bl_ui module per spec, and only UI-region panels get
  category/context (WINDOW popover panels reject categories).
- **First engine op**: `sculptcore.mask_flood_fill` (VALUE/INVERT) in the
  new `ops.py`, writing the engine mask column directly. Undo: the meshlog
  never sees whole-column writes, so `undo.py` gained *attribute-snapshot
  steps* (`push_attr` + `_ATTR_TAG` decode/free branch) restoring
  before/after blobs — the pattern for every future fill-style class-2 op.
  Multires sessions excluded (grid-channel masks, A4). Covered by a
  regression section incl. undo/redo round-trip and exit flush.
- **Mask menu** (`menus.py`): Invert/Fill/Clear via the new op, appended to
  `VIEW3D_MT_editor_menus` in-mode. Keymap: Alt-M clear, Ctrl-I invert
  (T3: covered chords 5 -> 10).

Also landed (regression 15/15): `sculptcore.face_sets_create` (MASKED —
reads mask + engine topology via the new `convert.mesh_topo_arrays`, all
corners past threshold join a fresh `maxFaceGroup()+1` set; FACE_I32
snapshot undo) and `sculptcore.subdivision_set` (sculpt_levels with clamp;
'UNDO' memfile step re-drives the engine via the depsgraph handler). Face
Sets menu added to the menubar; keymap gains Ctrl-0..5 and Alt-1/2
(T3 covered chords: 18 of 67).

Also landed (regression 16/16): `sculptcore.mask_filter` — all six vanilla
types (smooth/sharpen/grow/shrink/contrast +/-) as numpy passes over
unique-edge adjacency built from `convert.mesh_topo_arrays`; vanilla's
auto-iteration scaling (one pass per 50k verts); VERT_F32 snapshot undo.
Smooth/sharpen/contrast approximate vanilla's exact curves — flag for an
artist pass. The **mask edit pie** (all 8 vanilla slots, bound to A) and
the full Mask menu (fill/clear/invert + six filters, vanilla order) now
exist, plus a Sculpt menu whose Dynamic Topology entry is the scene-prop
toggle. Menubar reads View/Sculpt/Mask/Face Sets like vanilla.
T3: 19 of 67 chords covered.

Also landed (regression 17/17): `sculptcore.face_set_edit` GROW/SHRINK —
cursor pick via the engine raycast (`stroke._ray_from_coord` faceIndex),
falling back to the highest set for menu/headless invocations; one-ring
vert-connected grow, boundary shrink adopting an adjacent outside id;
FACE_I32 snapshot undo. Menu entries + Ctrl-W / Ctrl-Alt-W chords
(T3: 21 of 67). Caveat recorded: the pick uses the engine face index,
which can mismap after dyntopo freelist gaps (out-of-range picks fall
back safely).

Decisions: the face-sets edit pie (Alt-W) stays allowlisted — 3 of its 4
slots are visibility ops; a one-slot pie is worse than none. The
automasking pie (Alt-A) likewise — only cavity is mapped.

Also landed: **mask gestures** (`gestures.py`) — a modal base (ARMED from a
key like B, straight-to-DRAG from a mouse chord; GPU rubber band; ESC
cancels) with box + lasso ops. Box also applies from explicit xmin..ymax
props in execute() (vanilla's WM-gesture shape), which is what the new
GUI-only test `claudeMemory/tests/mask_gesture_test.py` drives (projection
needs a real region; ALL PASS + regression 17/17). Verts project via
numpy `perspective_matrix @ matrix_world`; lasso uses an even-odd polygon
test (bbox-prefiltered; boundary counts as inside). Chords mirror vanilla
verbatim incl. the quirky `B -> value 0.0`: B box, Ctrl-RMB lasso clear,
Ctrl-Shift-RMB lasso fill (T3: 25 of 67). Menu gains Box/Lasso Mask.
Audit-noted gaps: no symmetry passes, no front-faces-only option;
line/polyline variants not built (base supports them).

Remaining: line/polyline gesture variants (cheap on the same base), Sculpt
menu body (mostly class-3), T2 menu-by-menu item diff.

## Phase D — Keymap completion — DONE modulo allowlist (2026-07-19)

49 of 67 vanilla chords covered; all 18 remaining misses are allowlisted
class-3 features (expand, visibility, color sample/filter, voxel/dyntopo-R
edits, the two withheld pies). Landed this phase:
- Brush-asset shortcuts (data-driven `_BRUSH_ASSET_KEYS` table, verbatim
  vanilla incl. M's `use_toggle`) — `brush.asset_activate` and
  `brush.scale_size` verified working in-mode (bl_use_sculpt_paint).
  Brushes with no engine kernel (e.g. Crease Polish) activate but refuse
  to stroke — existing graceful path.
- Bracket size scaling, Shift-S smooth-stroke toggle, Alt-E stroke-method
  menu, Ctrl-F texture-angle radial, the six stencil_control chords
  (registered before the lasso/context-menu RMB items so poll fall-through
  matches vanilla), X color flip.
- **Mask brush toggle**: stroke operator gains mode 'MASK' (Alt-LMB /
  Ctrl-Alt-LMB), sharing the SMOOTH kernel-toggle path (flag renamed
  kernel_toggle; grab/face-set/autosmooth brush-type behavior correctly
  bypassed during toggles).
- **Asset shelf** (Phase E item, landed here): `SCULPTCORE_AST_brush_sculpt`
  from the unregistered View3DAssetShelf mixins, polling this mode;
  Shift-Space popover chord. Note `BrushAssetShelf.get_shelf_name_from_context`
  maps our mode to the *vanilla* shelf name (via get_brush_mode -> 'SCULPT');
  whether the header popup selector needs that map extended is a Phase E
  verification item.
Remaining for later: T5 event-sim spot checks; Shift-LMB semantics differ
(ours switches kernel, vanilla switches brush asset) — allowlisted.

## Phase E — Toolbar & asset shelf

- Toolbar: renders correctly in CUSTOM (verified by T4 crop 2026-07-19 —
  the single registered Brush tool draws and is active; the earlier
  "missing toolbar" read of the full-window shot was wrong). Remaining work
  is registering more tools as their engine ops land (mask box/lasso
  gestures first).
- Asset shelf: register a `VIEW3D_AST_brush_sculpt` analog polling the mode
  (brushes are shared assets, `brush.asset_activate` is class-1), plus the
  Shift-Space popover chord.

## Phase F — Properties (buttons) editor — mostly free (verified 2026-07-19)

Finding: the Properties Tool tab is not bl_context="tool" panels at all —
`buttons_main_region_layout` calls `ED_view3d_buttons_region_layout_ex(C,
region, "Tool")`, i.e. the *View3D sidebar layout* with mode-derived
contexts and a "Tool" category override. So the Phase B clones appear there
automatically (T4 crops: Brush Asset + Brush Settings render in CUSTOM's
Tool tab). No plumbing needed.

Fix landed: `Brush.direction` showed a dummy "Default" item in-mode because
`BKE_paintmode_get_active_from_context` had no OB_MODE_CUSTOM case (unlike
`BKE_paint_get_active`, which already resolved via
`BKE_object_custom_mode_uses_sculpt_paint`). Added the matching case
returning PaintMode::Sculpt — this drives the dynamic brush enum items
(direction and friends), so it likely fixes more than the visible symptom.

Polish items left: the tool draw_settings block at the top of the Tool tab
duplicates size/strength (vanilla shows only the tool label there);
Texture-tab brush-texture user verification.

## Phase G — Lock-in — T6 DONE (2026-07-19)

T6 landed as `claudeMemory/tests/ui_parity_test.py` (GUI-run gate; keymaps
are empty headless): every uncovered vanilla Sculpt chord must be bound to
an allowlisted operator or listed in the allowlist's new "chords" section
(the two withheld pies); every vanilla sculpt Tool-tab panel must have a
SCULPTCORE_* clone or a reasoned entry in the gate's PANEL_ALLOWLIST; the
three menubar menus must exist. ALL PASS (16 allowlisted chord misses).

The gate immediately earned its keep: it flagged the R / Ctrl-R dyntopo
chords (R now radial-controls scene.sculptcore_detail; detail_flood_fill
reclassified class-3) and the color/swatches panels (now cloned with a
has_color-gated poll — the engine COLOR kernel makes them meaningful).

Still open in this phase: T5 event-sim spot checks; archive a final full
T1–T4 run; artist stress pass (incl. the approximate mask-filter curves).

## Post-testing fixes (artist pass, 2026-07-20)

From live testing feedback:
- Tool header size/strength now uses `prop_unified` (pressure icons,
  unified routing) + the brush popup selector; new generic
  `ObjectModeType.bl_brush_asset_shelf` attr lets
  `get_shelf_name_from_context` resolve a custom mode's shelf.
- Brush child panels (Advanced/Texture/Stroke/Falloff/Display) exposed as
  individual header popovers like vanilla — registered child panels never
  render inside their parent's popover, so Accumulate was unreachable from
  the header.
- Accumulate wired to the engine: stroke passes `not use_accumulate` to
  `setNonAccum` (was hardcoded always-accumulate); smooth/mask toggles and
  grab keep inherent accumulation.
- Stroke spacing halved-density bug: vanilla spacing is a percentage of
  the *diameter* (`radius * spacing / 50`, #paint_space_stroke_spacing);
  ours walked `/ 100`.
- **Dyntopo now uses Blender's own detail settings** (`tool_settings.sculpt`
  detail_size / detail_percent / constant_detail_resolution /
  detail_type_method / detail_refine_method): `stroke.dyntopo_max_edge`
  ports the three #sculpt_detail.cc conversions (incl. the 0.4 relative
  scale + 0.4 min-edge factor); RELATIVE/BRUSH re-derive the bounds per
  remesh dab from the depth-dependent world radius; refine method maps onto
  the engine's DynTopoMode. `scene.sculptcore_detail` deleted (enable flag
  and remesh cadence remain addon props). The Dyntopo panel mirrors
  vanilla's (header checkbox, per-method detail prop, refine/detailing
  enums) and R runs `sculptcore.dyntopo_detail_size_edit`, which
  radial-edits the active detail prop by mode.
- "Adjust Strength for Spacing" implemented (`mapping.overlap_attenuation`,
  port of #paint_stroke_integrate_overlap: 1 / worst-case overlapping
  falloff sum, 10 phase offsets); folded into every dab's strength write.

- **Smooth brush explosion fix + vanilla smooth semantics**: the accumulate
  binding had made plain Smooth strokes run the engine's nonAccum snapshot
  mode (SMOOTH is `accumulable` engine-side) — re-basing a relaxation
  kernel against stroke-start positions blows up; the accumulate gate now
  also requires `sculpt_capabilities.has_accumulate`, like vanilla's UI.
  Smoothing (Smooth brush, Shift-toggle, autosmooth chain) now uses the
  BSMOOTH kernel (owner decision, superseding Q1b's plain-SMOOTH choice),
  and smooth strokes iterate per dab by strength — `int(strength*4)`
  full-strength passes + remainder (#iteration_strengths port), pressure
  and overlap folded python-side. Anchored/drag-dot smooth stays
  single-pass (preview path).

## Order & dependencies

0 → A → B → C → D (needs C's menus/pies) → E, F (independent, after B) → G.
Engine-op work surfaced in Phase 0 can proceed in parallel with A/B.
