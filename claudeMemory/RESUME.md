# Resume Pointer

Fast entry point for a new session. Everything is in git + `claudeMemory/`;
nothing lives only in a chat. Read this, then the two docs in §1, then continue.

Last updated: 2026-07-17. Current focus: **P8 multires — session wiring done;
next C2 (level UI) / C4 (undo payload)**.

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

## 3. The exact next tasks

- **C2** — level UI: `sculptlvl` ⇄ `Multires_setActiveLevel` (slot pointers
  change on switch — re-fetch `session.mesh_ptr`/`tree_ptr`, re-register the
  draw tree, reset the cached executor/meshlog wrappers).
- **C4** — undo payload via `Multires_serializeStore`/`_restoreStore` External
  chunks (with P6); today multires strokes ride the per-level meshlog only.
- P8 verification tail: production-asset render comparison; corpus (n-gon,
  creased, boundary-heavy cages; levels 1–6+); A4 grid paint-mask channel.

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
