# Resume Pointer

Fast entry point for a new session. Everything is in git + `claudeMemory/`;
nothing lives only in a chat. Read this, then the two docs in §1, then continue.

Last updated: 2026-07-17. Current focus: **P8 multires — session wiring**.

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
| **C1/C3 core — `sculptcore_addon/multires.py`** | import-exact, export ~1e-7, tear-free |

`multires.py` already does the full conversion (`modifier`, `build_engine`,
`build_map`, `import_displacement`, `export_bake`). Proven on a displaced
multires cube via `claudeMemory/scripts/p8_addon.py`.

## 3. The exact next task — session wiring (the last P8 piece)

Branch the addon session lifecycle in `sculptcore_addon/convert.py` to use
`multires.py` when `multires.modifier(ob)` is set:

- **`enter`**: `build_engine` + `build_map` + `import_displacement`; register
  `Multires_activeTree` (not the plain tree) for the external draw provider;
  set the multires modifier `show_viewport = False` (record prior state).
- **`flush` / `exit_`**: `export_bake` (engine top → `CD_MDISPS`) instead of the
  position-writeback path; restore `show_viewport` on exit.
- **`Session`**: hold the `Multires` handle + cage + cached `MultiresMap`; free
  both on exit.
- Keep the **plain-Mesh path untouched** — this branches, does not replace.

Then **GUI-test**: enter a real multires object, sculpt, exit, confirm vanilla
Blender shows the edit at various view levels. This touches the working sculpt
path, so do it with room to test.

Lower priority after: C2 (level UI `sculptlvl` ⇄ `setActiveLevel`), C4 (undo
payload via `Multires_serializeStore`/`_restoreStore`).

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
