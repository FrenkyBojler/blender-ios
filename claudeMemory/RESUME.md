# Resume Pointer

Fast entry point for a new session. Everything is in git + `claudeMemory/`;
nothing lives only in a chat. Read this, then the two docs in §1, then continue.

Last updated: 2026-07-18. Current focus: **usability pass COMPLETE** —
cursor overlay (ObjectModeType.draw_cursor seam + cursor.py), keymap pass
(Shift-smooth / Ctrl-invert via stroke `mode` enum, F / Shift-F radials),
and pressure (M4: per-stroke device dynamics via int-keyed ids + per-dab
`event.pressure`; engine fix — `execBrush` now applies `loadCommonProps`
with the device ctx). A real-pen feel test is still owed (simulation can't
carry pressure). Next: P8 A4 (grid paint mask), the P8 verification tail
(production asset, cage corpus), or P7 M2/M3 (engine-prop PropertyGroups +
brush panel).

---

## 1. Read these first

- **[plans/taskList.md](./plans/taskList.md)** — the P8 section is current; the
  `[~] C1/C3 core done` line names the exact remaining work.
- **[research/grid-correspondence.md](./research/grid-correspondence.md)** — §5a–§5e
  hold every validated P8 result and the reasoning (discrete-vs-limit base, the
  per-grid seam-tear finding, the nearest-neighbour vertex map). Load-bearing.
- `git log --oneline -25` — each commit message states what it did and how it was
  verified.

## 2. Where P8 stands (all committed, unpushed)

The whole multires conversion **mechanism is validated end-to-end**:

| Layer | State |
|---|---|
| P0 correspondence + numeric characterization | done |
| A1/A2 engine dumps (`Multires_fromLevelPositions` / `_levelPositionsOut`) | bit-exact |
| B / B2 Blender bake seams (`Object.multires_reshape_from_positions` / `_from_vert_positions`) | B2 seam-clean |
| A3 engine-sample ⇄ Blender-subdiv-vertex map (NN on zero-disp base) | bijective |
| C1/C3 core — `sculptcore_addon/multires.py` | ~1e-7 both ways, tear-free |
| **C1/C3 session wiring — `convert.py` branches** | headless + GUI verified |

`multires.py` does the full conversion (`modifier`, `build_engine`,
`build_map`, `import_displacement`, `export_bake`); `convert.enter/flush/exit_`
branch to it when a multires modifier is present (`_enter_multires` /
`_flush_multires`): stack import at `total_levels`, modifier `show_viewport`
suppressed in-mode and restored on exit, `Session` owns the `Multires` + cage
(active mesh/tree are stack-owned views), flush bakes into `CD_MDISPS`, and the
mid-stroke throttle only refreshes the draw provider (`convert.draw_refresh`).
Engine seam fix: `Multires_fromLevelPositions` rematerializes the seeded slot
(tree/normals were stale → raycast/brushes would miss). Verified by
`scripts/p8_session.py` (headless, ALL PASS) and a GUI pass (provider draws the
imported surface, dabs update live, vanilla multires shows the baked edit at
view levels 1–3 after exit).

C2 is done too: the modifier's `sculpt_levels` drives the engine level via a
`depsgraph_update_post` handler → `convert.set_multires_level` →
`_rebind_multires_views` (slot views re-fetched, wrappers + meshlog reset with
a generation bump, draw tree re-registered); enter honors it and
`_flush_multires` restores it after the top-level dump. N-panel "Multires"
slider. Gate: `scripts/p8_level.py`.

C4 is done: level-crossing undo works via per-stroke store-snapshot blobs
(`undo.push` serializes post-writeback; decode falls back to
`convert.multires_restore_blob` when the step's meshlog died), the level
switch itself undoes through its memfile property step + the depsgraph
handler, and the enabler was a C fix — `custom_mode_undo.cc` now has
`ut->poll = nullptr` (generic pushes fall to memfile; before, property edits
in-mode became inert CUSTOM steps and were unrecoverable) plus
flush-on-final-decode in `undo.decode` (re-asserts the engine after a
correct-order memfile restore below a custom step). Gates: `p8_c4.py` + the
full P6 suite (`claudeMemory/tests/`, run via `run_sync.py`).

## 3. The exact next tasks

- **A4** — grid paint-mask channel in/out (engine grid channel I/O + the
  `.sculpt_mask`-equivalent on grids).
- P8 verification tail: production-asset render comparison; corpus (n-gon,
  creased, boundary-heavy cages; levels 1–6+).
- Store-rewriting ops (down-refit, subdivide/delete) land with their
  features; their undo payload seam (`serializeStore` blobs) already exists.

Stroke-quality pass (after C4): dab spacing (2D screen-space StrokeSpacer,
projected per point — never space along the 3D hit polyline, it couples
density to the deforming surface) and deferred Mesh write-back (dabs/stroke
release only refresh the provider; the mode flush callback syncs on demand;
divergence guard keys off `session.blender_verts_num`). Gates:
`tests/deferred_flush_test.py`; interactive strokes can be driven with
`--enable-event-simulate` + `win.event_simulate` (dismiss the splash first —
it eats the first simulated click).

## 4. Environment gotchas (learned the hard way)

- **The addon runs from the build dir, not the source tree.** After editing any
  `scripts/addons_core/sculptcore_addon/*.py`, copy it to
  `build_windows_x64_clang_RelWithDebInfo/bin/5.3/scripts/addons_core/sculptcore_addon/`
  (or run the install) or the change is invisible.
- **Launch Blender with** `SCULPTCORE_PYTHON_PATH=extern/sculptcore/python` so the
  dev engine DLL loads.
- **Engine rebuild:** `cd extern/sculptcore && node make.mjs build python`.
  **Blender rebuild:** `claudeMemory\scripts\bl_env.bat cmake --build --preset
  relwithdebinfo --target blender`.
- **Crash debugging:** `cdb` (`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64`)
  with `sxe -c "...;q" av`; skip the benign first-chance `tbbmalloc` (rax=0) and
  asset-indexer AVs — condition on `@rax != 0` or the fatal module.
- **Submodule protocol** (see `extern/sculptcore/CLAUDE.md`): engine + parent are
  committed together (submodule first, then bump the gitlink). Do **not** push
  without asking.

## 5. Re-runnable validation harnesses (`claudeMemory/scripts/`)

Launch any with `blender --factory-startup --python <script>`; read
`%TEMP%/<name>.txt`.

- `p8_validate.py` — cross-engine CC base-surface characterization (§5a).
- `p8_roundtrip.py` — A1/A2 seed/writeback bit-exactness (§5b).
- `p8_pin.py` — per-grid convention pinning + seam-tear finding (§5c).
- `p8_vertcos.py` — dedup vertcos bake fidelity (§5d).
- `p8_export.py` — A3 map + export round-trip (§5e).
- `p8_addon.py` — **the full `multires.py` import→export round-trip**; run after
  any addon change to confirm no regression.
- `p8_session.py` — **multires through the real mode lifecycle** (enter/
  identity round-trip/sculpt/exit/modifier restore); the session-wiring gate.
  (Headless `--background` skips app timers — run these GUI-style, or via a
  driver that quits when the output file appears.)
- `p8_level.py` — **C2 sculpt-level switching** (handler follows
  `sculpt_levels`, view rebind, sculpt at a coarse level, flush restore,
  cascade into the bake).
- `p8_mundo.py` — **in-level stroke undo/redo on a multires session**
  (CUSTOM_MODE step → meshlog seek → re-bake; undo 2e-7, redo bit-exact).
  Timer-run ops push no undo steps — the harness pushes a "setup" step so
  the stroke has a real boundary below it.
- `p8_c4.py` — **level-crossing undo/redo** (strokes at two levels + a
  sculpt-level switch; meshlog fast path, blob fallback, level follows the
  history).
- Run any timer-style harness with `run_harness.py`, and the synchronous
  P6 tests (`claudeMemory/tests/custom_undo_*.py`) with `run_sync.py`
  (hard-exits — `wm.quit_blender` can block on the save prompt).
