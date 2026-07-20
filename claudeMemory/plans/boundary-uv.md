# P11 — Boundary System & UV Reprojection Integration

Goal: interface SculptCore's boundary system with Blender end to end —

1. migrate Blender's seam/sharp edge flags into the engine's boundary bool
   attributes (and back out through topology rebuilds and undo),
2. **build** the engine's UV/corner slide-reprojection (the bsmooth.sbrush:21
   follow-up — it does not exist yet) so smoothing-induced tangential vertex
   drift no longer makes textures swim, wired into BSMOOTH-family dabs and
   dyntopo's tangential smooth,
3. expose the engine's UV projection unwrap (`generateUVFromSeams`) in the
   Blender UI, with real UV write-back to the Mesh,
4. verify/fix UV-chart boundary flag derivation and its handling by the
   constraint system (dyntopo `FeatureViews`, bsmooth `VERT_CLASS`), including
   the ≥3-junction pinning case.

Directly unblocks and completes the P9 Q1b BSMOOTH decision (mapping.py:45 —
`'SMOOTH' → BSMOOTH` is already mapped, awaiting feature-edge transfer).

## Validated background (2026-07-20)

**Engine boundary model** (`source/mesh/boundary.{h,cc}`): source-of-truth
bool edge attrs `.boundary.edge.seam` / `.sharp` / `.projected` (persistent);
derived TEMP flags `.boundary.edge.polygroup` / `.uvchart`; per-vert
classification bitmask `.boundary.vert.class` (`BC_*` bits, `boundary.h:70`),
rebuilt lazily by `recomputeDirty` (needs live topology). `setEdgeFlag`
(`boundary.cc:151`) marks edge+verts dirty. Consumers:
`bsmooth.sbrush`/`featurealign.sbrush` (vclass gates wTan/wNor),
dyntopo `FeatureViews` (`dyntopo.h:540+`, live edge-bool views; splits
propagate source flags and mark dirty — `dyntopo.h:849,970,1150`).
Executor recompute sites: stroke-start for BSMOOTH/FEATURE_ALIGN
(`brush_executor.h:956,1316`), per-dyntopo-dab (`:1494`), stroke end
(`endDynTopoStroke`, `:1688`).

**UV reprojection does NOT exist.** The only "reproject" in the tree is the
quad-remesh geometric surface snap (`remesh/extract/reproject.cc`). Nothing in
brush/dyntopo touches UVs when vertices slide.

**Key executor mechanics to reuse:**
- `coPrevStorage` (`brush_executor.h:229,712-760`): capacity-indexed pre-dab
  position snapshot, maintained for every `needsCoPrev` kernel (bsmooth,
  smooth, featurealign have it). This is the "old position" source.
- `node.affected_verts`: appended by generated kernels for every vert a dab
  moved (`bsmooth.brush.gen.h:112`).
- Meshlog element store is domain-generic incl. CORNER
  (`meshlog_base.h:405-473`, `LogChunkElems`); `AttrSaver<CORNER>` stamp gate
  exists (`attr_saver.h:53`). Corner rows must be captured before UV writes so
  reprojection is undoable.
- Non-dyntopo strokes freeze topology per-stroke (CSR ring-1 neighbors);
  corner/radial walks need a thaw. Dyntopo strokes keep topology live
  (`keepTopoThawed`).

**Blender side** (`scripts/addons_core/sculptcore_addon/`): no edge attrs
bridged (`convert.py:342` `_DOMAIN_TO_ENGINE` has no EDGE — engine edges are
derived by `Mesh_fromArrays`, no index correspondence). Current Blender attr
names: `uv_seam`, `sharp_edge` (EDGE/BOOLEAN). Engine has `find_edge(v1,v2)`
(`mesh.cc:147`). `Mesh::generateUVFromSeams(marginMilli)` (`mesh.cc:984`)
allocates a **new** unique `uv[.NNN]` corner layer — for Blender we need a
caller-named target instead. The addon loads the active UV map into engine
layer `"uv"` (`convert.py:309`, draw provider reads it by that name) *and*
bridges the same layer under its Blender name via `_load_bridged_attrs` — two
engine copies that must be kept in sync on UV-project.

---

## Work items

### Engine (extern/sculptcore)

| # | Item | Where | Size |
|---|---|---|---|
| E1 | **UV reprojection core**: `mesh/uv_reproject.{h,cc}` — `reprojectVertUVs(MeshBase *m, span<const int> verts, OldPos oldCo)`; per moved vert, per UV layer (FLOAT2 corner attrs tagged `AttrUse::UV`): group the vert's corners into wedges by old-UV equality at the vert (actual UV discontinuity, NOT the derived `.uvchart` flag — immune to staleness); project the new position onto each wedge face's old corner triangle `(v, next, prev)` (old ring positions via `oldCo` fallback current co); closest point → clamped barycentric → interpolate the face's old corner UVs; two-phase (compute all, then write) so order can't matter. Skip degenerate faces / empty wedges. GTest `test_uv_reproject`. | new files | M |
| E2 | **Executor wiring** (`Brush::reproject_uvs` bool, reflected): per-dab when topology is live (dyntopo strokes) — reproject `affected_verts` against `coPrevStorage` right after `exec()`; on frozen (non-dyntopo) strokes — accumulate `{vert → first-seen coPrev}` per dab, reproject once at `endStep()` (thaw, capture, reproject, node GPU-dirty) before the meshlog step closes. Gate on `cmd.needsCoPrev` (snapshot validity). Corner-row undo capture via `AttrSaver<CORNER>` + `meshLog->elemStore(CORNER)` immediately before writes. | `brush.h`, `brush_executor.h`, `brush/bindings` | L |
| E3 | **Dyntopo tangential-smooth reprojection**: `DynTopoParams::reproject_uvs`; in the `do_smooth` block (`dyntopo.h:1075`) capture `sverts` old positions (already in hand), after the Jacobi write call E1 (topology live). Fire corner undo capture through `MeshCallbacks::onCornerChange` (or direct meshlog capture as in E2). Extend `test_dyntopo_smooth`. | `dyntopo.h`, `dyntopo/bindings.cc` | M |
| E4 | **Junction pinning**: `BC_JUNCTION = 1<<7` set by `recomputeDirty` when `domCount >= 3`; bsmooth/featurealign kernels skip such verts entirely (a cube-corner must not erode). sbrush edits + `node make.mjs codegen`. | `boundary.{h,cc}`, kernels | S |
| E5 | **UV-chart flag verification**: ctest reproducing dyntopo split/collapse across a UV-chart boundary — assert `FeatureViews` honors the boundary during the dab (propagated flags) and `recomputeDirty` re-derives `.uvchart` correctly on the fresh geometry afterwards; fix propagation if the derived TEMP flag is not copied to split children. Also cover `computeUvChartBoundary`'s 2-face-only limitation (mesh-boundary edges are handled topologically — assert no regression). | tests | M |
| E6 | **C-API for the addon**: in `mesh_c_api.cc` — `Mesh_edgeCount(m)`; `Mesh_writeEdgeFlagsByVerts(m, name, edge_verts /*2·n engine-vert-order = Blender vert order on enter*/, values, n)` (via `find_edge` + `boundary::setEdgeFlag`); `Mesh_readEdgeFlags(m, name, r_edge_verts, max)` → count, engine vert-pair per flagged edge (Python maps via `Mesh_toArrays`' vert_map); `Mesh_recomputeBoundary(m)`; `Mesh_generateUVFromSeams(m, name, marginMilli)` (named-target variant of `mesh.cc:984`). | `mesh_c_api.cc` | M |

### Addon (scripts/addons_core/sculptcore_addon)

| # | Item | Where | Size |
|---|---|---|---|
| A1 | **Seam/sharp migration**: on enter — read `uv_seam`/`sharp_edge` + `edges.foreach_get("vertices")`, seed via `Mesh_writeEdgeFlagsByVerts` (Blender vert index == engine vert index at enter), then `Mesh_recomputeBoundary`. On the topology-rebuild flush — after `calc_edges=True`, `Mesh_readEdgeFlags` per flag, map engine verts through `vert_map`, build a sorted-pair → Blender-edge-index lookup (numpy int64 keys + searchsorted), recreate both attrs. Fast path: skip (topology unchanged ⇒ Blender edges/flags still valid). | `convert.py`, `engine.py` decls | M |
| A2 | **UV write-back**: `_flush_uv` (engine `"uv"` → active UV layer) gated by `session.uv_dirty`; the UV-project operator sets it and also syncs the bridged copy (`Mesh_writeAttr` under the Blender layer name) so a later topology rebuild restores the *new* UVs. Creates a UV layer when none exists. | `convert.py`, `session.py` | S |
| A3 | **UI**: operator `sculptcore.uv_project_from_seams` (calls `Mesh_generateUVFromSeams(b"uv", margin)` + A2 sync + undo push) with a margin prop; panel section (Dyntopo panel or a new "Boundary" panel) with the button; brush toggle **Reproject UVs** → `sc_brush.reproject_uvs` (reflected field, set in `mapping.apply_brush_settings`); dyntopo **Reproject UVs** scene prop → `DynTopoParams.reproject_uvs`. | `ui.py`, `ops` module, `mapping.py`, `props.py`, `stroke.py` | M |
| A4 | **Undo**: new `'CORNER_F32x2'` kind in `undo.py` `_ATTR_KINDS` (whole-column UV snapshot) for the UV-project operator; stroke-time reprojection rides the meshlog (E2/E3 corner capture). Edge flags need no per-stroke undo (set only at enter / by Blender edit-mode ops between sessions), but the rebuild flush (A1) must run inside the existing DECODE_ACTIVE_STEP seek model unchanged. | `undo.py` | S |
| A5 | **BSMOOTH parity re-check** (P9 Q1b follow-through): with seam/sharp transferred, A/B BSMOOTH on marked-feature meshes — feature edges must hold under Shift-smooth/autosmooth; record the outcome in `mapping.py`'s comment. Dyntopo `do_smooth` pins feature verts (already engine-side) — regression via a marked-cube headless test. | tests, `mapping.py` comment | S |

## Order

1. **E1** (pure, unit-testable) → **E6** (C-API, testable via ctypes without Blender).
2. **A1** (flag migration; enables everything boundary-aware) → **A5** check.
3. **E4 + E5** (constraint-system correctness).
4. **E2 → E3** (reprojection wiring; E3 reuses E1+E2's capture helper).
5. **A2 → A3 → A4** (UV project UI + undo, needs E6's named variant).

## Verification

- Engine: `node make.mjs build native` + `node make.mjs test` — new
  `test_uv_reproject`, extended `test_dyntopo_smooth`, new chart-boundary
  dyntopo test; existing bsmooth/dyntopo gates stay green.
- `test_uv_reproject` cases: interior tangential slide on a textured grid
  (UV moves so the *sampled texture point* stays fixed, analytic check);
  seam vert (two wedges, each interpolates within its chart); normal-only
  motion (UV unchanged within eps); degenerate ring (no NaNs, UV kept).
- Addon headless (claudeMemory/tests): seam/sharp round-trip enter→exit
  byte-exact; round-trip through a dyntopo stroke + undo (flags survive
  rebuild); uv-project operator writes back + undoes; BSMOOTH holds a marked
  crease; reproject-uvs stroke leaves texture visually stable (checksum of
  sampled UVs at fixed surface points) and undoes exactly.
- Rebuild DLL (`node make.mjs build python`) and GUI sanity pass per
  root CLAUDE.md launch recipe.

## Outcome (implemented 2026-07-20)

All items landed and verified. Notable deviations / discoveries:

- **E5 found a real engine bug**: edge *collapse* restored corner UVs verbatim
  per corner (kill-fan corners kept v_kill's UVs, untouched keep-fan corners
  kept v_keep's), so every collapse created micro UV discontinuities → spurious
  derived chart edges everywhere, and tore real chart boundaries (35 chain ends
  instead of 2 in the stress run). Fixed with a wedge-aware UV blend in
  `edge_collapse.h`: the killed faces holding both endpoints yield one
  `(uv_kill, uv_keep)` pair per wedge; every corner at the survivor is set to
  its wedge's `keep*(1-blend) + kill*blend`, matched by proximity.
  `test_dyntopo_uvchart` gates it (chain simple+connected through Both-mode
  dabs; incremental recompute == from-scratch re-derivation).
- **`Mesh_writeCornerFloat2Attr` didn't tag `AttrUse::UV`**, so the addon-seeded
  engine `uv` layer was invisible to chart derivation and reprojection (only
  the bridged copy was tagged). Now tags UV.
- **Attr-snapshot undo steps must flush on the leave decode too** (`undo.py`
  `_decode_attr`): the undo destination can be a step the custom type never
  decodes (mode-enter memfile boundary), which left the restored engine column
  invisible on the Mesh.
- Junction pinning uses a new `BC_JUNCTION = 1<<7` (domCount >= 3) and both
  smooth kernels skip such verts outright.
- E2's frozen-stroke path defers reprojection to `endStep()` via a
  `{vert -> first-seen coPrev}` map; corner undo capture reuses the meshlog
  CORNER element store with `AttrSaver<CORNER>` custom bits.
- Numbers: BSMOOTH holds a sharp-marked crest at 0% erosion (vs 66% height
  loss unmarked); stroke UV drift 0.0 with reprojection vs 0.0296 without
  (Blender path), 3e-8 vs 0.014 (dyntopo tangential smooth, engine test).

Tests added: engine `test_uv_reproject`, `test_dyntopo_uvchart`, junction case
in `test_boundary`; Blender `edge_flag_test.py`, `uv_project_test.py`,
`bsmooth_boundary_test.py`, `uv_reproject_stroke_test.py`
(claudeMemory/tests). All engine + addon suites green (the 6 pre-existing
GPU/env ctest failures on this machine are unchanged).

## Open questions / decisions taken

- "UV project" = `generateUVFromSeams` planar unwrap exposure (operator), and
  "wired into bsmooth/dyntopo tangent smooth" = the new E1–E3 slide
  reprojection. The `projection` uniform (volume-preservation) is unrelated
  and already auto-exposed via the manifest props.
- Wedge detection uses actual old-UV discontinuity, not `.uvchart` flags —
  reprojection stays correct even where derived flags are stale mid-stroke.
- Non-dyntopo strokes reproject at stroke end (one thaw), not per dab —
  avoids perturbing the frozen-CSR stroke state and the per-dab thaw cost;
  UVs are correct at commit (and at every flush the user can observe).
- Blender crease (`crease_edge`, float) is out of scope: the engine boundary
  system has no float crease consumer; tracked under multires-convert §6.
