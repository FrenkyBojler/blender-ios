# Plan — SculptCore Addon Skeleton

**Goal.** The addon itself: package layout, mode registration, session
management, the interactive stroke operator, keymap/tools/UI, and the v0
end-to-end slice (enter → stroke → see result → undo → exit → save) using
only Tier-1 infrastructure — flush-to-Mesh drawing and memfile undo — so
every later plan (draw provider, wrapped undo, brush table, multires) plugs
into a working host.

**Dependencies.** [mode-infra.md](./mode-infra.md) (must have Phases A–B),
[python-bindings.md](./python-bindings.md) (engine importable),
[mesh-convert.md](./mesh-convert.md) B1–B3. Consumes
[brush-mapping.md](./brush-mapping.md) M1 for its first stroke.

---

## 1. Package layout

```
scripts/addons_contrib-style dev location (or repo `sculptcore_addon/` +
symlink into scripts/addons during development):

sculptcore_addon/
  __init__.py        # bl_info, register/unregister, ObjectModeType subclass
  engine.py          # sculptcore package import, lib load + ABI guard,
                     # session registry {object_session_token: Session}
  session.py         # Session: engine Mesh/SpatialTree/Brush/CommandExecutor,
                     # meshlog handle, generation counter, index maps
  convert.py         # mesh-convert plan workstream B
  multires.py        # multires-convert plan workstream C
  mapping.py, props.py, ui.py    # brush-mapping plan
  stroke.py          # SCULPTCORE_OT_brush_stroke modal operator
  draw.py            # Phase 0 flush throttle; later: provider registration
  keymap.py, tools.py
  lib/               # sculptcore package + native shared lib + .pyi stubs
```

The `sculptcore` Python package (bindings plan Workstream E) vendors into
`lib/`; `engine.py` is the only importer (single load point, version check,
clear error UI if the native lib is missing).

## 2. The mode class

```python
class SculptCoreMode(bpy.types.ObjectModeType):
    bl_idname = "sculptcore.sculpt"
    bl_label = "SculptCore"
    bl_icon = 'SCULPTMODE_HLT'
    bl_object_types = {'MESH'}
    bl_keymap = "SculptCore Mode"
    bl_use_custom_undo = True   # inert until undo-integration lands

    def enter(self, context, ob):   # convert.enter → session registry
    def exit(self, context, ob):    # flush + free session
    def flush(self, ob):            # convert.flush (fast/slow path)
    def refresh(self, context, ob): # rebuild after foreign undo (generation++)
```

Enter validation (v1 rules, from
[../research/sculpt-modifier-coupling.md](../research/sculpt-modifier-coupling.md)):
refuse shape-keyed meshes; warn-and-proceed when enabled non-multires
modifiers exist (results hidden while sculpting); multires detection →
`multires.py`.

## 3. The stroke operator (the core of the addon)

`SCULPTCORE_OT_brush_stroke` — modal operator, structure cribbed from vanilla
paint stroke operators (`paint_stroke.cc` behavior, reimplemented in Python):

- **invoke:** ray-cast the cursor via `region_2d_to_origin_3d`/`ray_cast` on
  the *engine's* spatial tree (`castRay` binding — not `scene.ray_cast`, which
  sees the evaluated mesh) → stroke start; `mapping.apply_brush`;
  `executor.beginStep(dyntopo_enabled)`; add modal handler + timer for
  airbrush.
- **modal (MOUSEMOVE/pressure):** accumulate mouse samples; spacing/jitter/
  smooth-stroke sampling host-side (`stroke_method`); per dab:
  `brush.pushDeviceInput(pressure/tilt)`, radius unprojection,
  `executor.applyDab(program, center, normal, radius, dyntopo_params, seed)`;
  then draw update (Phase 0: throttled `convert.flush_positions` +
  `ED_region_tag_redraw`-equivalent; later: provider dirty-notify + redraw
  tag only).
- **release:** `executor.endStep()` (+`endDynTopoStroke` if used);
  Tier-1: plain `bpy.ops.ed.undo_push`-equivalent via operator `'UNDO'`
  semantics — actually: the operator finishing with `{'FINISHED'}` and
  `bl_options={'UNDO'}` triggers a memfile push, which triggers `flush` — v0
  undo correctness for free. Tier-2 swaps this for `undo_push_custom`.
- Symmetry v1: X-mirror by double-dabbing mirrored centers (engine-side
  symmetry support to be checked later — keep host-side initially).

Cursor: `SpaceView3D.draw_handler_add` radius circle (same data as vanilla
paint cursor, minimal version).

## 4. Keymap, tools, UI

- Keymap `"SculptCore Mode"`: LMB → stroke, ctrl → invert, shift → smooth
  program, F/shift-F radius/strength radial (v1: plain float pop-ups; radial
  gesture later), ctrl+Z passthrough to undo.
- Tools: one `WorkSpaceTool` per supported `sculpt_brush_type` group keyed to
  `context.mode == "sculptcore.sculpt"` (tool system keys off the context
  string — free once mode-infra B5 works). v0: a single "Brush" tool;
  per-type toolbar follows brush-mapping M1 breadth.
- UI: header mode dropdown comes from mode-infra B4; N-panel / Properties
  panels from brush-mapping M3; dyntopo toggle panel reading `Sculpt`
  DNA fields.

## 5. Change list / order of work

| # | Milestone | Contents |
|---|---|---|
| S1 | Loadable skeleton | package, engine import + ABI guard, mode registers/unregisters, enter/exit with positions-only conversion. Verify with remote REPL. |
| S2 | First stroke | stroke.py with DRAW brush via mapping M1; Phase-0 flush throttle; `'UNDO'` memfile undo works; cursor overlay. |
| S3 | Usability pass | keymap complete, single tool, brush panel, invert/smooth modifiers, pressure. |
| S4 | Lifecycle hardening | object switch / workspace switch / file open-close / addon disable while in mode / `refresh` generation handling; ASAN session. |
| S5 | Integration points | swap in draw provider (draw-integration D6), wrapped undo (undo-integration B), multires (multires-convert C), brush breadth (mapping table). |

S1–S4 need only Tier-1 Blender infra. S5 items land as their plans complete —
each behind a capability check so the addon runs (degraded) without them.

## 6. Verification

- Scripted end-to-end (remote REPL): enter → 50 synthetic dabs → exit →
  assert mesh changed plausibly → undo → assert restored.
- Interactive checklist per milestone (tablet pressure, all keymap entries,
  mode dropdown, tool switching).
- Lifecycle matrix (S4) incl. two objects alternating, workspace switch
  mid-stroke-idle, quit with unsaved session.
- Performance gates: v0 flush path usable at 100k verts; post-S5 provider
  path interactive at 1M+ (draw-integration verification owns the numbers).

## 7. Risks / notes

- Python-side per-dab overhead: dab rate is bounded by spacing (not vsync);
  `applyDab` is one bindings call — fine. Mouse-sample math in numpy if
  needed.
- Modal-operator + undo interplay: `bl_options={'UNDO'}` push timing with a
  modal operator needs verification (vanilla paint pushes explicitly);
  fallback is an explicit `bpy.ops.ed.undo_push(message=...)` on release.
- `castRay` needs the multires-active mesh's tree when multires is on —
  session must swap tree handles on level switch (session.py owns this).
- Addon disable while a session is live: `unregister` must force-exit all
  objects in the mode (mode-infra guarantees the C-side call; addon must
  survive it being re-entrant with exit()).
