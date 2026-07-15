# Plan — Multires Conversion (MDISPS ⇄ SculptCore Displacement Grids)

**Goal.** Sculpt multires meshes in the SculptCore mode: on enter, convert
Blender's multires data (`CD_MDISPS`) into SculptCore's own subdivision +
displacement-grid system; on exit/flush, bake edits back into `CD_MDISPS` so
the unchanged multires modifier keeps working everywhere else. **The multires
modifier itself is ignored while the mode is active** (its viewport display is
suppressed; we never touch `SubdivCCG`).

**Dependencies.** [mesh-convert.md](./mesh-convert.md) (base-mesh conversion —
the multires cage rides that path), [mode-infra.md](./mode-infra.md).
Runs after both have their first phases landed.

---

## 1. Background (validated 2026-07-15)

### Blender multires

- `MDisps` — `DNA_meshdata_types.h:215`: per-**loop** customdata layer
  (`CD_MDISPS = 19`); each loop owns one square grid, `totdisp` samples,
  `disps` float3 array, side = `2^(level-1)+1`
  (`multires_side_tot[]`, `multires.cc:52`), sample order `y*side + x`
  (`BKE_ccg.hh:75`). Quad = 4 grids, n-gon = n grids.
- **Displacement space:** tangent space relative to the **Catmull-Clark limit
  surface** of the base mesh — tangent frames from
  `multires_reshape_evaluate_base_mesh_limit_at_grid`
  (`multires_reshape.hh:267`) / `BKE_multires_construct_tangent_matrix`
  (`BKE_multires.hh:224`). MDISPS stores only the **top level** (`totlvl`).
- `MultiresModifierData` — `DNA_modifier_types.h:1218`: `lvl` (viewport),
  `sculptlvl`, `renderlvl`, `totlvl`. Simple subdivision is deprecated
  (converted on load, `BKE_multires.hh:235`) — CC semantics only.
- Reshape entry points: `multiresModifier_reshapeFromObject`
  (`BKE_multires.hh:169`), `_reshapeFromCCG` (`:177`,
  `multires_reshape.cc:113`); internal context
  `MultiresReshapeContext` (`multires_reshape.hh:33`) with
  `_assign_final_coords_from_vertcos` (`:298`) →
  `multires_reshape_object_grids_to_tangent_displacement` (`:374`).
- Grid paint mask: `CD_GRID_PAINT_MASK` / `GridPaintMask`
  (`DNA_meshdata_types.h:231`) — per-loop float grids, same sizing.

### SculptCore multires (`extern/sculptcore/source/subdiv/`)

- Own discrete CC refiner (`Refiner`, `subdiv.h:62`; crease/boundary rules;
  level 1 splits every n-gon into n quads) with CSR `StencilTable`
  (`subdiv.h:27`) — canonical, GPU-bit-identical position arithmetic.
- `GridsStore` (`grids.h:62`): per-cage-face-corner Ptex-convention lattices;
  channel 0 = **per-level, frame-relative float3 tangent-space displacement**
  ("delta against the smoothed previous level in the level's frame",
  `grids.h:3`); level-L grid side = `2^(L-1)` (`grids.h:66`).
- `Multires` orchestrator (`multires.h:49`): `init(cage, maxLevel)` (`:58`),
  `setActiveLevel`/`materialize` (`:71/:75`) → a real `mesh::Mesh` per level
  (so tree/executor/meshlog/draw all apply unchanged), `writeback(level)`
  (`:82`) re-expresses edited positions into store deltas,
  `downRefit(level)` (`:92`). C-API: `Multires_new`, `_writeback`,
  `_activeMesh`, `_activeTree`, `_serializeStore`/`_restoreStore`
  (`c-api/subdiv_c_api.cc:19-121`).

### The key mismatch → the key decision

Blender stores one top-level grid set in limit-surface tangent frames;
SculptCore stores per-level cascaded deltas in its own smoothed-base frames.
**Do not convert frames — convert positions.** Both systems can produce/absorb
absolute object-space positions of the top-level vertices; round-trip through
those:

- **Import:** evaluate Blender's final multires positions per grid sample
  (limit surface + tangent frame + `disps` — the standard reshape-context
  evaluation), map grid samples → SculptCore level-`totlvl` mesh vertices,
  then run SculptCore's writeback cascade to distribute into per-level deltas.
- **Export:** materialize SculptCore's top level → absolute positions → map
  back to grid samples → `multires_reshape_assign_final_coords_from_vertcos`
  + `object_grids_to_tangent_displacement` (i.e. the
  `multiresModifier_reshapeFromObject` machinery) to re-bake `CD_MDISPS`.

This sidesteps frame-convention and per-level-semantics differences entirely;
the cost is one full-resolution position pass each way, which enter/exit can
afford.

## 2. The sample correspondence (the real work)

Both systems use one grid per face corner with quadrant conventions
(Blender per-loop MDISPS; SculptCore Ptex `__faceindex`, `subdiv.h:41`), but:

- side lengths differ: Blender `2^(l-1)+1` (vertex-sampled, shared
  boundaries duplicated per grid) vs SculptCore `2^(L-1)` noted as the lattice
  step (`gridVerts` layout to be pinned down in P0 below);
- level numbering and boundary-sample ownership (seam duplication vs
  `GridLink` seam mates, `grids.h:144-148`) differ;
- n-gon handling matches (n quads/grids per n-gon) but corner ordering must
  be verified.

**P0 investigation task:** write the exact bijection
`(loop, y, x, blender_level)` ⇄ `(sc_grid, u, v, sc_level)` including boundary
sample dedup, validated numerically (subdivide a cube in both, compare
positions at zero displacement — both are CC, so base-surface samples must
agree to float tolerance; any systematic mismatch reveals a convention error
before displacement is involved). Deliverable: a research note
`../research/grid-correspondence.md` + a C-API mapping helper.

Watch item from the SculptCore survey: crease semantics — Blender creases via
`multiresModifier` use OpenSubdiv rules; SculptCore has its own sharp/boundary
crease rules (`boundary::EDGE_SHARP`). Zero-displacement comparison will
expose divergence; if the discrete CC bases differ materially, the
position-based round-trip still *works* (it bakes whatever surface SculptCore
produced) but detail will shift on meshes with creases — document the
limitation or map creases in P0.

## 3. Change list

### Workstream A — SculptCore C-API

| # | Where | Change | Size |
|---|---|---|---|
| A1 | `subdiv_c_api.cc` | `Multires_fromLevelPositions(cage, maxLevel, positions[], map)`: init + seed store by writing top-level absolute positions and running the writeback cascade (top → 1). | M |
| A2 | `subdiv_c_api.cc` | `Multires_levelPositionsOut(level, positions[])`: materialize + dump absolute positions in mapping order. | S |
| A3 | grid mapping helper | The P0 bijection as a C entry: fills index arrays mapping Blender grid samples → level-mesh vertex ids (both directions), given cage topology in Blender layout. | M |
| A4 | `subdiv_c_api.cc` | Grid-mask channel: `GridsStore::addChannel` 1-float channel for `.sculpt_mask`-on-grids (`CD_GRID_PAINT_MASK` in/out), same mapping. | S |

### Workstream B — Blender-side bake seam

`multiresModifier_reshapeFromObject` needs a source *object*; we have raw
positions. Two options — decide at implementation:

- **B-py:** build a temporary high-res Mesh in Python and call the existing
  reshape operator path. Zero C changes, heavy allocation.
- **B-c (preferred):** small C hook (RNA function on `Object` or a
  `bpy.ops`-reachable utility, ~100 lines in `multires_reshape.cc` area):
  `multires_reshape_from_positions(object, level, positions)` — wraps
  `_context_create_from_object` + `_assign_final_coords_from_vertcos` +
  `object_grids_to_tangent_displacement`. This is a generic, upstreamable
  utility ("reshape multires from a flat position array"), consistent with
  the minimal-modification strategy.

### Workstream C — Addon integration

| # | Change | Size |
|---|---|---|
| C1 | `enter`: detect multires (modifier present + `CD_MDISPS`, mirroring `sculpt_multires_modifier_get`, `paint.cc:2241` semantics but independent of `OB_MODE_SCULPT`); evaluate current MDISPS → top-level positions (via the reshape-context evaluation exposed in B, or first-cut: depsgraph-evaluate the multires modifier at `totlvl` and read the eval mesh); A3 map; A1. Suppress modifier viewport display (`show_viewport = False`, restore on exit). | M |
| C2 | Level UI: map `sculptlvl` ⇄ `setActiveLevel`; subdivide/delete-level operators deferred to a later plan (v1 sculpts existing levels only). | S |
| C3 | `flush`/`exit`: A2 → B bake → MDISPS updated; base-mesh (`cage`) edits from `downRefit`/base sculpting are out of scope v1 — cage positions written back only if SculptCore modified them. | M |
| C4 | Undo: multires store snapshots ride `Multires_serializeStore`/`_restoreStore` as the External-chunk payload — coordinate with [undo-integration.md](./undo-integration.md). | S |

## 4. Order of work

1. **P0 correspondence investigation** (blocks everything; produces the
   research note + A3).
2. A1/A2 + zero-displacement round-trip test in sculptcore's harness.
3. C1 import path — multires mesh visible and sculptable in the mode.
4. B + C3 export path — MDISPS bake; vanilla Blender shows the edits after
   exit.
5. A4 masks; C2 level switching; C4 undo payload.

## 5. Verification

- **Zero-displacement round trip:** multires mesh with empty MDISPS → enter →
  exit → MDISPS still ~zero (< 1e-5), base untouched.
- **Identity round trip:** sculpted multires asset (production file) → enter
  → exit with no strokes → rendered result (F12, multires at `renderlvl`)
  visually identical / positions within tolerance.
- **Edit round trip:** stroke at top level → exit → vanilla sculpt mode +
  viewport at various `lvl` values show the edit correctly; undo after exit
  (memfile) restores.
- N-gon cage, creased-edges cage, boundary-heavy cage (grid seams) in the
  corpus; level 1 through 6+ (memory).
- Level switching mid-session preserves detail (SculptCore writeback
  losslessness gate).

## 6. Risks / open questions

- **Crease/boundary rule divergence** (§2 watch item) — could shift detail on
  creased meshes; quantify in P0.
- **Simple-subdivision legacy files** — handled by Blender's own on-load
  conversion to CC; no work here, but keep a corpus case.
- Memory: top-level position arrays for level-6+ multires are large but
  transient; stream per-face-batch if it becomes a problem.
- `GridPaintMask` ⇄ mask channel ordering shares the A3 map — one more
  consumer of the single-source-of-truth rule from mesh-convert.
