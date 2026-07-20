# UI-Parity Tooling (SculptCore mode vs vanilla Sculpt mode)

Goal: make the SculptCore custom mode's UI match vanilla sculpt mode across
every surface — Properties (buttons) editor tabs, View3D header + popovers,
Tool tab / N-panel, toolbar, Sculpt/Mask/Face Sets menus, right-click context
menu, and hotkeys.

Ground truth for "what vanilla sculpt shows" is a mix of static code
(`scripts/startup/bl_ui/space_view3d_toolbar.py`, `space_view3d.py`,
`properties_paint_common.py`, `space_toolsystem_toolbar.py`, and the keyconfig
`scripts/presets/keyconfig/keymap_data/blender_default.py`) and *runtime*
behavior (poll results, popover contents, tool-system state). The tooling
below captures the runtime truth so parity can be diffed mechanically instead
of eyeballed.

## Existing tooling (kept as-is)

- `remote_repl.py` — live GUI REPL, main-thread-safe. Backbone for all UI
  tooling; UI state only exists in a real window.
- `run_harness.py` + `p8_*.py` — GUI-launch, write-output-file, quit pattern.
- `addon_regression.py` — headless engine-level assertions.

None of these can see the UI. New tools:

## T1 — Poll-matrix dump (`ui_poll_matrix.py`) — DONE

Built and verified. Run (GUI required; keyconfigs/screen only exist there):

```
set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
blender.exe --factory-startup --python claudeMemory/scripts/ui_poll_matrix.py
  -> %TEMP%/ui_poll_matrix.txt (summary) + ui_poll_matrix.json (full matrix)
```

First findings (2026-07-19): `context.mode` is `'CUSTOM'` in our mode, so
every `bl_context`-gated panel (".sculpt_mode", ".paint_common") is
structurally invisible regardless of poll — the whole Tool-tab panel set
(brush settings/stroke/falloff/texture/display, dyntopo, symmetry, options,
remesh, overlay popover) is missing; the addon currently exposes only 4
N-panel replacements. AttributeError in a poll is reported as "NA" (the
panel system's bl_context prefilter would have excluded it).

Iterate every registered `bpy.types.Panel` / `Menu` / `Header` subclass and
record `{idname, space, region, category, label, parent, order}` plus the
result of `poll()` under two contexts: vanilla sculpt mode and SculptCore
mode (same object, same window layout). Dump JSON for both; the diff is the
authoritative list of UI surfaces sculpt mode has that ours lacks (Properties
editor Tool tab, header popovers, N-panel Tool panels, etc.).

Run via `run_harness.py` (needs GUI for real `context.area`/`region`
overrides per space type — iterate the screen's areas and override per
panel's `bl_space_type`).

## T2 — Layout introspection capture (`ui_introspect.py`) — DONE

Built and verified (43/44 targets; the one failure is
VIEW3D_PT_tools_brush_clone, a texture-paint-only panel whose poll would
exclude it in sculpt mode anyway). Same GUI invocation as T1; output
`%TEMP%/ui_introspect.txt` + `ui_introspect.json`.

Implementation (differs from the original monkeypatch idea): a capture
panel registered in the View3D sidebar's "Item" tab (the default-active tab;
`Region.active_panel_category` is read-only from Python) calls each target
class's `draw()` against a shim whose `.layout` is a fresh sub-layout, then
records `layout.introspect()`. The shim resolves methods via
`inspect.getattr_static` so staticmethods/classmethods on the paint-panel
mixins bind correctly. A panel layout is required as host — header layouts
reject the `layout.panel()` sub-panels the brush panels use. Targets never
need to be visible or bl_context-matched; captures run under vanilla SCULPT
for the vanilla set, under CUSTOM for the addon panels, and under both for
VIEW3D_HT_header / VIEW3D_MT_editor_menus.

The JSON is item-level: operators with full arguments
(`bpy.ops.sculpt.mesh_filter(type='SMOOTH')`), properties as RNA paths
(`Sculpt.lock_x`), labels, tooltips, and the nesting structure — the parity
spec to diff addon panels against.

Output: per-panel/per-menu machine-readable item lists (operators, props,
labels, order) for vanilla sculpt surfaces → the parity spec our addon
panels are diffed against, at the item level. Also works for the context
menu (`VIEW3D_PT_sculpt_context_menu`) and the Sculpt/Mask/Face Sets menus.

## T3 — Keymap dump + diff (`keymap_dump.py`) — DONE

Built and verified. Same GUI invocation as T1 (in `--background` the
"Sculpt" keymap exists but is empty — keyconfig content only loads with a
window). Output: `%TEMP%/keymap_dump.txt` (chord diff) + `keymap_dump.json`.

First findings (2026-07-19): 5 chords covered (LMB/Ctrl/Shift strokes,
F/Shift-F radial), ~60 vanilla chords unbound: mask ops + pies (A, Alt-A,
Alt-M, Ctrl-I, B, gestures), subdivision levels (Ctrl/Alt-digits), brush
asset shortcuts (V/S/P/I/G/K/C/M/...), bracket size scaling, stroke-method
and stencil controls, context menu (RMB/APP -> VIEW3D_PT_sculpt_context_menu),
asset shelf popover (Shift-Space), dyntopo/remesh (R, Ctrl-R). Note vanilla
Shift-LMB now uses `brush_toggle: 'SMOOTH'` (brush-asset toggle) where ours
uses `mode: 'SMOOTH'`. The per-tool "3D View Tool: Sculpt, ..." keymaps are
listed by name in the JSON for when the toolbar is ported.

Serialize keymap items (idname, type, value, ctrl/alt/shift/oskey, key
modifier, `properties` as a dict, active state) for the vanilla "Sculpt"
keymap and related (e.g. "Paint Curve", tool keymaps from the toolbar), and
for whatever keymap the custom mode registers. JSON out, plain diff. Covers
hotkey parity (F/Shift-F radius/strength, Ctrl invert, Shift smooth, mask
shortcuts, brush cycling, etc.).

Not headless-capable (verified: in `--background` the "Sculpt" keymap is
present but has zero items); a T6 regression hook needs a GUI launch via the
`run_harness.py` pattern.

## T4 — Screenshot harness (`ui_screenshot.py`) — DONE

Built and verified. Run (GUI; fixed geometry keeps shots comparable):

```
set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
blender.exe --factory-startup --window-geometry 0 0 1600 1000 \
    --python claudeMemory/scripts/ui_screenshot.py
  -> %TEMP%/ui_shot_{SCULPT,CUSTOM}_{full,view3d,properties}.png + manifest
```

Deterministic setup (sphere, view_all, Properties on the Tool tab, sidebar
open), captures the full window per mode and crops the View3D/Properties
areas from it with numpy (rects scaled by image/window ratio, so Windows
DPI scaling is handled). The splash is suppressed by setting
`preferences.view.show_splash = False` at script-import time — that runs
before the first window draw, so it works even with --factory-startup.
Limits: the sidebar shows its default-active "Item" tab
(Region.active_panel_category is read-only), and the brush cursor sits
wherever the OS mouse is.

First shots (2026-07-19) make the gaps visible directly: CUSTOM header has
only a "View" menu (vs View/Sculpt/Mask/Face Sets + symmetry/falloff/etc.
popovers), the mode dropdown shows the raw "sculptcore.sculpt" id, there is
no toolbar and no brush asset shelf, and the Properties Tool tab shows only
the addon's minimal Brush block instead of Brush Asset + full Brush
Settings.

## T5 — Event-simulate hotkey runner (`ui_event_sim.py`)

Launch with `--enable-event-simulate` and drive `window.event_simulate` (the
pattern in `tests/python/bl_run_operators_event_simulate.py`) to verify keys
actually dispatch to the intended operators inside the custom mode — the only
way to test keymap *resolution* (modal keymaps, tool keymaps, fallthrough to
Object Mode keymap) rather than keymap *contents*.

## T6 — Regression hook

Once T1/T3 exist, add a "UI parity" section to `addon_regression.py`
asserting the poll-matrix and keymap diffs are empty modulo a curated
allowlist (intentionally unsupported features). Keeps parity from regressing
as panels are ported.

## Build order

T1 and T3 first (cheap, define the work list), T2 next (defines per-panel
content spec), T4 whenever visual checking starts, T5 last (only needed when
hotkey wiring lands). T6 after T1/T3 stabilize.
