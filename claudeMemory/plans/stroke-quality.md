# P9 — Stroke Quality & Parity (reference-app insights)

Goal: close the six high-value gaps identified in
[../research/webgl-app-reports-insights.md](../research/webgl-app-reports-insights.md)
(distilled from the reference TypeScript app's reports in
`webgl-app-framework-reports/`): executor neighbor cache, smooth-kernel
choice, dyntopo cadence, spline stroke smoothing, symmetry, and
anchored/drag-dot stroke methods.

All engine seams used here are already reflected — verified in
`extern/sculptcore/source/brush/brush_executor.h`:
`setNeighborMode` (`:335`, bound `:287`),
`beginPreviewDab`/`rollbackPreviewDab`/`commitPreviewDab` (`:1708`–`1753`,
bound `:282`–`:286`; exercised in `source/debug/script.cc:1527`), and the
`BSMOOTH` kernel (`brushes/types.h`). **No engine or Blender C changes** —
this plan is addon-only (Python), which is why it gets its own plan instead
of growing P4/P7.

Depends on: P4 (stroke operator), P7 M1 (mapping), P6 (undo bracketing).
All landed.

---

## Work items

| # | Item | Files | Size |
|---|---|---|---|
| Q1a | `setNeighborMode(1)` (CSR ring-1 neighbor cache): set on executor construction as the reference app does; A/B first (timing + result identical) since our default is engine-default. | `stroke.py` `_ensure_executor` | S |
| Q1b | BSMOOTH vs SMOOTH: A/B on an open-boundary mesh (P8 corpus grid) — if BSMOOTH is boundary-preserving (matching Blender's smooth), switch the SMOOTH mapping entry, the Shift-smooth stroke kernel, and the autosmooth chain kernel. Keep SMOOTH if behavior is worse. | `mapping.py:45`, `stroke.py` (smooth-mode kernel ~`:305`, autosmooth program ~`:202`) | S |
| Q2 | Dyntopo cadence: remesh at its own spacing along the stroke, not every dab. Track accumulated stroke arc length `stroke_s` (advance by the spacer step); `due = stroke_s - last_dyntopo_s >= dyntopo_spacing`; when due → `apply_dyntopo_dab` and advance, else → the plain program path (reference passes `params ?? 0`). Spacing default derived from detail size; expose in the Dyntopo N-panel. Once Q4 lands: decide `due` **once on the primary dab**, cache for the mirror images (otherwise the primary starves the mirrors of the spacing budget). | `stroke.py` (operator modal ~`:406`–`:430`), `ui.py` | M |
| Q3 | Spline stroke smoothing: port the reference's pure-math layer (`stroke_math.ts` — `crToBezier` centripetal α=0.5, `evalCubic`, `subCubic`, `arcLengthWalk` with 32 chords) into a new dependency-free `stroke_math.py`; upgrade `StrokeSpacer` from linear-polyline walk to the spline walk: 2D screen-space control points, 1-segment lookahead (a segment emits only once its right neighbor exists), first control point emits one raw dab immediately, `walk_carry` continues across abutting segments (replaces `residual`), trailing segment flushed with a right-clamp on release. Downstream stays unchanged: each emitted 2D point is still projected onto the surface. | new `stroke_math.py`, `stroke.py` (`StrokeSpacer` ~`:35`–`:70`, operator wiring) | M |
| Q4 | Symmetry (plane-mirror): port the `SymAxisMap` table (8 axis-bit combos → per-component sign-flip vectors; X+Y+Z = 7 reflections) into a new `symmetry.py`. Applied in the operator **after** the spacer/driver (driver stays mirror-agnostic): primary dab first, then per mirror — flip the center and direction vectors (normal, view ray) by the sign vector, **re-raycast along the mirrored view ray** to snap the center onto the surface (skip for grab-class: mirror the anchor + cursor directly). We pass an explicit world radius per dab, so the reference's rendermat-folding trick for flip-invariant pixel radius is unnecessary — reuse the primary's world radius. Per-mirror state kept separate: grab anchor/cursor per mirror index; dyntopo-due shared per Q2. Read the shared sculpt symmetry the vanilla mode uses (DNA `mesh.symmetry` X/Y/Z flags; verify the RNA path — `Mesh.use_mirror_x/y/z` — at implementation). N-panel Symmetry section. Radial symmetry: deferred (no reference implementation; parity-checklist entry). | new `symmetry.py`, `stroke.py`, `ui.py` | L |
| Q5 | Anchored / Drag-Dot stroke methods via the engine preview-dab API. Branch on `brush.stroke_method`: **ANCHORED** — first input must hit the surface (else refuse the stroke); anchor center + the camera view vector are captured at anchor time; later inputs project the cursor ray onto the camera-facing plane through the anchor; radius = screen-space drag length unprojected (Blender's anchored semantics); one dab per input, pinned at the anchor. **DRAG_DOT** — one dab per input at the live cursor. Both bypass the spacer entirely (no spline, no trailing flush). No-compounding: before each new dab `rollbackPreviewDab()`, apply inside `beginPreviewDab(center, radius)`; stroke end commits exactly one live dab via `commitPreviewDab()` **before** `endStep()`; cancel (Esc) rolls back, ends the step, skips the undo push. Mirrors (Q4) share one preview bracket (`extendPreviewDab` per image) so one rollback reverts the whole group. DOTS/SPACE map to the existing spacer path; AIRBRUSH/LINE/CURVE deferred to the parity checklist. | `stroke.py` (operator + new anchored/dot paths) | L |

## Order

1. **Q1a + Q1b** — independent quick wins; each is an A/B plus a small edit.
2. **Q3** — pure-math layer + spacer swap (isolated; unit-testable without
   the engine).
3. **Q2** — cadence rides the arc-length bookkeeping Q3 introduces
   (`stroke_s` falls out of the walk).
4. **Q4** — symmetry over the finished single-image pipeline.
5. **Q5** — stroke methods last: they interact with both symmetry (shared
   preview bracket) and undo (commit-before-endStep ordering).

## Verification

- **Q1a**: timing A/B on a dense-mesh SMOOTH stroke; identical vertex
  results (bit-exact or documented epsilon) with the cache on.
- **Q1b**: open-boundary grid — boundary verts must not collapse inward
  under the chosen kernel; interior smoothing equivalent. Record the
  decision in `mapping.py`.
- **Q2**: instrument remesh calls — N dabs at spacing s must trigger
  ~`strokeLen / dyntopo_spacing` remeshes, not N; dyntopo result quality
  spot-checked against the every-dab baseline.
- **Q3**: unit tests for `stroke_math.py` (arc-length accuracy vs analytic
  curves, carry continuity across segments — no clustering at joints);
  A/B a jittery synthetic input: spline path curvature must be bounded
  where the polyline path is angular; slow/fast event-rate strokes stay
  near-identical (extends the existing 40-vs-8-move gate).
- **Q4**: mirrored stroke on a symmetric mesh ⇒ geometry stays symmetric
  (max asymmetry ~0); X, X+Y, X+Y+Z combos; grab with symmetry; dyntopo
  with symmetry remeshes all images on the same samples; undo exact.
- **Q5**: port the reference's no-compounding regression
  (`sculptcore_anchored_dragdot.test.ts` pattern) as
  `claudeMemory/tests/stroke_methods_test.py`: a 2-point "direct" stroke vs
  a 5-point "wander" stroke ending at the same place, checksum Σ|v|² —
  direct displaces (> 1e-4), |wander − direct| < 10 % of the direct delta,
  undo restores both. Plus: anchored refuses an empty-space start; cancel
  leaves the mesh unchanged; full P6 undo suite re-run green (preview
  bracket sits inside the meshlog step).
- All of the above headless via `run_sync.py` conventions (NaN-proof
  guards); final GUI pass for feel (symmetry + anchored under real mouse
  input via `--enable-event-simulate`).

---

## Outcome (implemented)

All six items landed, addon-only, and are verified headless. New modules:
`stroke_math.py` (Q3), `symmetry.py` (Q4). New tests under `claudeMemory/tests/`:
`stroke_math_test.py`, `stroke_spacer_test.py`, `dyntopo_cadence_test.py`,
`symmetry_test.py`, `stroke_methods_test.py` (all green; the full P6 undo suite
and `addon_regression.py` 12/12 stay green).

Two deviations from the plan, both forced by engine behavior:

- **Q1b — kept SMOOTH (did not switch to BSMOOTH).** BSMOOTH's boundary
  awareness only preserves *marked* feature edges (sharp/seam/poly-group/
  UV-chart); open mesh boundaries (1-face edges) are "handled topologically
  elsewhere," so on the open-boundary grid the A/B specified, BSMOOTH and SMOOTH
  collapse the boundary identically (0.17786 both). With no marked features (and
  no edge-flag transfer to the engine boundary attrs yet) BSMOOTH only adds a
  per-stroke boundary refresh with no parity gain. Q1a *was* adopted
  (`setNeighborMode(1)`: bit-identical, marginally faster).

- **Q4 — mirror by reflecting the resolved primary hit, not by re-raycasting.**
  The plan (following the reference app) called for re-raycasting the mirrored
  view ray. Our engine's `spatial::castRay` reconstructs the hit position
  imprecisely — a vertical ray down the sphere's center returns `(0, -0.195,
  0.98)` instead of the pole — so re-raycasting the mirrored ray is *not*
  reflection-equivariant (0.258 asymmetry on a unit sphere). Reflecting the
  resolved primary center+normal directly is exact (1.3e-6, the mesh's own
  symmetry floor) and matches vanilla Blender's "mirror the operation" model.
  The grab path already mirrors anchor/cursor directly, so it was unaffected.

Known follow-ups (parity checklist): dyntopo is not remeshed inside the
anchored/drag-dot preview path (deform only); AIRBRUSH/LINE/CURVE fall back to
the spacer path; radial symmetry deferred; the engine `castRay` position
imprecision affects dab-center accuracy generally (engine-side fix, out of scope
for this addon-only plan).
