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

## T1 — Poll-matrix dump (`ui_poll_matrix.py`)

Iterate every registered `bpy.types.Panel` / `Menu` / `Header` subclass and
record `{idname, space, region, category, label, parent, order}` plus the
result of `poll()` under two contexts: vanilla sculpt mode and SculptCore
mode (same object, same window layout). Dump JSON for both; the diff is the
authoritative list of UI surfaces sculpt mode has that ours lacks (Properties
editor Tool tab, header popovers, N-panel Tool panels, etc.).

Run via `run_harness.py` (needs GUI for real `context.area`/`region`
overrides per space type — iterate the screen's areas and override per
panel's `bl_space_type`).

## T2 — Layout introspection capture (`ui_introspect.py`)

`UILayout.introspect()` returns a JSON description of everything a layout
contains, but a real layout only exists inside a draw callback. Approach:
monkeypatch the target class's `draw` to run the original and then append
`self.layout.introspect()` to a capture dict, force a redraw (tag the area,
spin the event loop via a timer), restore. Driven over the REPL or as a
harness script.

Output: per-panel/per-menu machine-readable item lists (operators, props,
labels, order) for vanilla sculpt surfaces → the parity spec our addon
panels are diffed against, at the item level. Also works for the context
menu (`VIEW3D_PT_sculpt_context_menu`) and the Sculpt/Mask/Face Sets menus.

## T3 — Keymap dump + diff (`keymap_dump.py`)

Serialize keymap items (idname, type, value, ctrl/alt/shift/oskey, key
modifier, `properties` as a dict, active state) for the vanilla "Sculpt"
keymap and related (e.g. "Paint Curve", tool keymaps from the toolbar), and
for whatever keymap the custom mode registers. JSON out, plain diff. Covers
hotkey parity (F/Shift-F radius/strength, Ctrl invert, Shift smooth, mask
shortcuts, brush cycling, etc.).

Headless-capable (`--background` can read `wm.keyconfigs`), so this can also
become a regression-suite section.

## T4 — Screenshot harness (`ui_screenshot.py`)

REPL/harness-driven `bpy.ops.screen.screenshot(filepath=...)` with a
deterministic scene, window size, and layout script; optionally crop to an
area's rect (areas expose x/y/width/height) with numpy/`bpy.data.images`.
Capture the same views in vanilla sculpt and SculptCore mode for visual
side-by-side (Claude reads the PNGs). Complements T1/T2: catches things
introspection misses (icons, enabled/greyed state, layout density).

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
