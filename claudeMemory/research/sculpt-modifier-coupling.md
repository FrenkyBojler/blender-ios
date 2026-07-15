# Research — Where Sculpt Mode Couples to the Modifier Stack / Geometry Nodes

*Survey of `main`, 2026-07-15. Answers: where does Blender interface sculpt
mode's mesh structures with the modifier stack and geometry nodes, what would
SculptCore need from each point, and how long can we avoid supporting
sculpting with active modifiers?*

**Decision context.** The SculptCore mode will **not** support sculpting a
mesh while deform modifiers / geometry nodes are active (deferred). It **will**
support multires, by ignoring the multires modifier and using SculptCore's own
subdivision + displacement (see [../plans/mesh-convert.md](../plans/mesh-convert.md)).

All line numbers verified 2026-07-15; they will drift.

---

## 1. The interface points

### 1.1 Session setup from the evaluated object — `paint.cc`

- `sculpt_update_object()` — `blenkernel/intern/paint.cc:2359-2470`. The
  workhorse: pulls the evaluated mesh via
  `BKE_object_get_evaluated_mesh_unchecked(ob_eval)` (`:2372`), grabs
  `mesh_eval->runtime->subdiv_ccg` (`:2390`, how multires grids reach the
  session), ensures the PBVH (`:2392`).
- The **`deform_modifiers_active` branch** — `paint.cc:2394-2435` — is where
  active deform modifiers force crazyspace / eval-mesh coordinates onto the
  PBVH. *This entire branch is what we are deferring.*
- Depsgraph hooks: `BKE_sculpt_update_object_before_eval` (`paint.cc:2472`,
  called from `mesh_data_update.cc:1057`) frees the PBVH before re-eval;
  `BKE_sculpt_update_object_after_eval` (`paint.cc:2508`, called from
  `mesh_data_update.cc:951-955`) re-syncs after. Editor-side entry:
  `BKE_sculpt_update_object_for_edit` (`paint.cc:2540`).

### 1.2 SculptSession deform state — `BKE_paint.hh`

- `deform_modifiers_active` (`BKE_paint.hh:409`), `deform_cos` (`:411`),
  `deform_imats` (`:413`), `shapekey_active` (`:384`),
  `vert_normals_deform`/`face_normals_deform` (`:419-420`).
- Detector `sculpt_modifiers_active()` — `paint.cc:2316-2357`: false when
  dyntopo/multires active (`:2320`); true for non-locked shape keys (`:2325`)
  or enabled deform modifiers (`:2348-2351`). Multires and shape-key virtual
  modifiers are explicitly skipped (`:2337-2346`) — **multires is already
  outside the deform-modifier path**, which is exactly the shape we want.

### 1.3 Crazyspace — `crazyspace.cc`

- `BKE_crazyspace_build_sculpt` — `blenkernel/intern/crazyspace.cc:404-486`.
  Copies the mesh, re-runs every deform modifier, derives per-vertex inverse
  deform matrices/quaternions so brush translations on the deformed shape can
  be mapped back to original coordinates.
- Applied on stroke write-back by `PositionDeformData::deform()` —
  `editors/sculpt_paint/sculpt.cc:8238-8269` (and in undo restore,
  `sculpt.cc:1206,1230`, `sculpt_undo.cc:1463,1627`).
- Multires with `sculptlvl > 0` early-returns empty deform data
  (`crazyspace.cc:347-351`) — multires *disables* crazyspace.

### 1.4 Modifier-stack short-circuit during eval — `mesh_data_update.cc`

- `mesh_calc_modifiers`: sculpt flags at `:298-302`; the main gate at
  `:389-424` — in sculpt mode, **constructive modifiers are already
  hard-disabled** ("Not supported in sculpt mode"), and everything after the
  multires modifier is disabled once it runs (`multires_applied` latch,
  `:587-589`). Deform modifiers still evaluate and feed the PBVH through
  crazyspace.
- So sculpt mode does *not* hide modifiers; it selectively runs deform-only
  ones. Geometry nodes have **no special-case code**: a GN modifier that
  deforms sets `deform_modifiers_active`; one that changes topology falls
  under the constructive-modifier gate.

### 1.5 Write-back / flush — `pbvh.cc`, `multires.cc`, `ed_util.cc`

- Position source: `cache_source_get()` — `blenkernel/intern/pbvh.cc:894-920`.
  With no deform data it is `PositionSource::Orig`: the PBVH reads/writes the
  **original mesh's position array directly** (asserted identical at
  `sculpt.cc:1225,8231`). This is the fast path we keep conceptually.
- Multires flush: `multires_flush_sculpt_updates` — `multires.cc:277-330` →
  `multiresModifier_reshapeFromCCG` (`:325`) pushes CCG edits into `CD_MDISPS`.
- Editor flush: `ED_editors_flush_edits_for_object_ex` —
  `editors/util/ed_util.cc:269-316`; honors `SculptSession::needs_flush_to_id`
  (`BKE_paint.hh:496`), called before memfile undo encode
  (`memfile_undo.cc:81`), render (`render_internal.cc:883`), etc. This is the
  flush seam the custom-mode design generalizes
  ([../design/addon-custom-modes.md](../design/addon-custom-modes.md)).

### 1.6 Draw fallback — `BKE_sculptsession_use_pbvh_draw`

- `paint.cc:2788-2816`, consulted at `draw_context.cc:682`, workbench/EEVEE/
  overlay engines. For a Mesh PBVH it returns *false* (draw the evaluated mesh
  instead — full modifier re-eval per stroke step) when
  `shapekey_active || deform_modifiers_active || external_engine`. Grids
  always draw from the PBVH. This is the "slow path when modifiers active"
  users know from vanilla sculpt.

---

## 2. What SculptCore needs from each point

| Blender interface point | SculptCore mode |
|---|---|
| `deform_modifiers_active` branch + crazyspace (`paint.cc:2394-2435`, `crazyspace.cc:404`) | **Skip entirely.** Mode-enter poll refuses (or warns and ignores) enabled non-multires modifiers. |
| Constructive-modifier gate (`mesh_data_update.cc:389-424`) | Nothing to do — already disabled in sculpt-like modes; our mode reads the *original* mesh, so evaluated-mesh contents don't matter during sculpting. |
| `subdiv_ccg` / grids PBVH (`paint.cc:2390,2752`) | **Not used.** SculptCore has its own CC subdivision + displacement grids; we convert `CD_MDISPS` directly on enter/exit and ignore the multires modifier at runtime (disable its viewport visibility on enter, restore on exit). |
| `multires_flush_sculpt_updates` (`multires.cc:277`) | Replaced by our own MDISPS write-back in the mode's `flush()` (see mesh-convert plan). |
| `needs_flush_to_id` / `ED_editors_flush_edits_for_object_ex` (`ed_util.cc:269`) | Generalized as the `flush()` callback of `ObjectModeType` — the one C hook the design already plans. |
| Shape keys (`shapekey_active`, `paint.cc:2385`) | **Deferred** with the rest of the deform path; poll refuses meshes with shape keys for v1. |
| `BKE_sculptsession_use_pbvh_draw` fallback | Irrelevant to us (custom mode never enters that gate); our draw path has its own gate (see draw-integration plan). |

## 3. How long can we avoid the modifier/GN coupling?

**Indefinitely for correctness; revisit only for feature parity.** The
coupling exists solely to let users sculpt *through* a live deform stack
(armature posing while sculpting, GN deformers, shape keys). Nothing in save,
undo, render, or mode switching requires it:

- With the mode active, the depsgraph still evaluates the object normally
  (our mode doesn't touch eval); renders and other viewports see the
  evaluated mesh built from the Mesh ID we keep flushed.
- The price of deferral is a UX rule: entering the SculptCore mode with
  enabled deform modifiers/GN/shape keys either (a) is refused by the poll, or
  (b) proceeds with a warning that modifier results are not shown while
  sculpting (we draw the base mesh via our own path). Recommendation: **(b)
  for visibility-only cases, (a) when the stack would make results
  misleading** — decide during addon-skeleton implementation.
- Supporting it later means reimplementing the crazyspace contract on the
  addon side: build `deform_imats`-equivalents from
  `BKE_crazyspace_build_sculpt` (exposed to the bridge), apply inverse
  matrices to SculptCore's output translations before writing to the Mesh.
  It is additive — no v1 design decision blocks it. Dyntopo-with-deformers is
  excluded even in vanilla Blender, so parity pressure is low.

**Watch item:** vanilla sculpt's behavior here is user-visible (Blender
*does* allow deform modifiers in sculpt mode, slowly). Feature-parity
comparisons will flag this; document it as a known v1 limitation in the addon.
