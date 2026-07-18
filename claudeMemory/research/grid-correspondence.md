# Research — Multires Grid-Sample Correspondence (P8 P0)

Status: **investigation complete (2026-07-17)**; the bijection below is
specified with one convention flag (intra-grid transpose) and one topology
assumption (corner-enumeration parity) left to be **pinned numerically** by the
zero-displacement round-trip test (§5). Blocks P8 A1/A2/A3.

Source surveys that back every claim here: SculptCore
`extern/sculptcore/source/subdiv/{grids,subdiv,multires}.{h,cc}` +
`c-api/subdiv_c_api.cc`; Blender `blenkernel/intern/multires*.cc`,
`multires_reshape*.{cc,hh}`, `BKE_ccg.hh`, `subdiv_inline.hh`,
`DNA_meshdata_types.h`.

---

## 1. The two layouts, side by side

| Property | Blender `CD_MDISPS` | SculptCore `GridsStore` |
|---|---|---|
| Grid unit | one per **loop** (face corner) | one per **cage face corner** |
| Grids per face | quad → 4, n-gon → n | quad → 4, n-gon → n |
| Grid enumeration | face order, then corner/loop order (`context_init_lookup`, `multires_reshape_util.cc:74-105`) | face-id order, then `c.next` corner order (`grids.cc:120-127`) |
| Samples per edge (level L) | `side = 2^(L-1)+1` (`multires_side_tot[]`, `multires.cc:52`; closed form `multires.cc:369`) | `w = sideForLevel(L)+1 = 2^(L-1)+1` (`grids.h:66`, `grids.cc:30`) |
| Samples per grid | `side²` (`totdisp`) | `w²` |
| In-grid index | `y*side + x`, x=fast (`CCG_grid_xy_to_index`, `BKE_ccg.hh:75`) | `v*w + u`, u=fast (`grids.cc:68`) |
| Sample `(0,0)` | the loop's **corner vertex** | the corner vertex (`subdiv.cc:351`) |
| Opposite diagonal `(max,max)` | ptex `(0,0)` = **face center** (`grid_uv_to_ptex`, `subdiv_inline.hh:34`) | face point (`subdiv.cc:354`, level-1 `gv[3]`) |
| Axis directions | grid u/v → ptex via `ptex_u=1-v, ptex_v=1-u` | `+u` along corner edge `c.e[cc]`, `+v` along prev corner edge `c.e[cp]` (`subdiv.h:41`) |
| Boundary samples | **replicated** across the grids that share them (face center in all n; each edge midline in 2) (`multires_reshape_vertcos.cc:44-92`) | **replicated**; aliases via `GridLink`/`seamMates` (`grids.h:18`, `grids.cc:229`) |

**Key consequence — no level offset.** Both use `2^(L-1)+1` samples per grid
edge and identical level numbering. SculptCore level L ⇄ Blender level L. (The
`2^L+1` figure that appears on the Blender side is the *whole-ptex-face*
subdivided-mesh resolution used by `foreach_subdiv_geometry` for the vertcos
scatter — `= 2·2^(L-1)+1`, two corner grids sharing the center row — not the
per-grid side.)

**Key consequence — the grid anchor agrees.** Both put `(0,0)` at the shared
corner vertex and the far diagonal at the face center. So the per-sample map
between corresponding grids is either **identity** `(x,y)=(u,v)` or a single
**transpose** `(x,y)=(v,u)`; nothing more exotic (no flip, because both origins
are the corner, not an edge midpoint).

---

## 2. The bijection

For grid sample at Blender `(loop L_b, x, y)` and SculptCore `(grid g, u, v)` at
a shared level:

```
g            = corr_grid[L_b]              # face/corner enumeration map (§3)
(u, v)       = TRANSPOSE ? (y, x) : (x, y) # intra-grid convention (§4)
side == w    = 2^(level-1) + 1             # identical, no offset
```

Both directions are total on the *replicated* sample sets (each system stores
every boundary sample in every grid that touches it), so the map is a plain
per-sample index permutation — no dedup bookkeeping needed at the grid layer.
Shared vertices are written more than once with the same value; both engines
already tolerate that (Blender: `vertcos_foreach` scatters to all replicas;
SculptCore: writeback visits every slot, `multires.cc:216`).

### `corr_grid` (§3 detail)
Both enumerate grids as *(face in order) × (corner in order)*. If the cage's
per-face corner order is preserved through `Mesh_fromArrays` (it is — corners
are passed in Blender loop order), then `corr_grid` is the **identity on a flat
grid index**: Blender grid index == SculptCore grid index. The face-corner
*start offsets* also line up because both accumulate corner counts in the same
face order (`face_start_grid_index` vs the `gridOf[cc]` running counter). **To
verify:** that both walk a face's corners in the same rotational direction (CW
vs CCW). If they disagree, `corr_grid` within a face is `corner' =
(n - corner) % n` — the numeric test (§5) exposes it immediately.

---

## 3. Interchange format: absolute per-grid positions

Do **not** convert frames (Blender = limit-surface tangent; SculptCore =
per-level smoothed-base tangent). Round-trip **absolute object-space positions
of the top level**, per the plan's key decision. The natural interchange buffer
is a **CCG-shaped array**: `abs[grid][y*side + x]` = float3, grid in loop order.
Blender consumes/produces exactly this shape through the reshape machinery; the
per-grid map (§2) rewrites it into SculptCore's `(g,u,v)` slots.

### Export (SculptCore → MDISPS)
1. SculptCore: `Multires_setActiveLevel(totlvl)`, materialize the top-level
   mesh, read absolute vert positions.
2. **A2** `Multires_levelPositionsOut(level, out)` — dump absolute positions in
   a specified order. Emit in **CCG per-grid order** `[grid][y*side+x]` using
   `levelGridVertsOut` `(g,u,v)→vid` and the §2 transpose, so the buffer needs
   no Blender-side remap.
3. Blender: **B** `multires_reshape_from_positions(object, level, abs[])` wraps
   the canonical sequence (`multires_reshape.cc:28-47`):
   `context_create_from_modifier(top_level)` → `store_original_grids` →
   `ensure_grids` → `assign_final_coords_from_*` → `smooth_object_grids_with_details`
   → `object_grids_to_tangent_displacement` → `context_free`.
   Prefer the **CCG-shaped** assign
   (`assign_final_coords_from_ccg`, `multires_reshape_ccg.cc:18-78`, sample
   `vert = grid_area*grid + y*grid_size + x`) over the subdiv-vertex
   `assign_final_coords_from_vertcos` — the CCG order is per-grid and matches our
   buffer directly, avoiding the `foreach_subdiv_geometry` vertex numbering.

### Import (MDISPS → SculptCore)
1. Blender: get top-level absolute positions. First cut — depsgraph-evaluate the
   multires modifier at `totlvl` (`BKE_multires_create_mesh`, or
   `BKE_object_get_evaluated_mesh`), which yields the subdiv-vertex-ordered
   dedup mesh. To get **per-grid** absolute samples instead, read the grids
   through a reshape context (`eval_limit + tangent·disp`, the forward of
   `object_grid_element_to_tangent_displacement`, `multires_reshape_util.cc:770`)
   into the CCG-shaped buffer.
2. **A3** grid map applies §2 to scatter `abs[grid][y*side+x]` →
   SculptCore level-`totlvl` mesh vertex ids (`levelVertGridCoordsOut` reverse
   map, first-owner per replica; write every replica).
3. **A1** `Multires_fromLevelPositions(cage, maxLevel, positions[], map)` — seed
   the store by writing those top-level absolute positions and running the
   writeback cascade top→1 (`Multires::writeback`, `multires.cc:543`), which
   distributes them into per-level frame-relative deltas.

---

## 4. Convention flags to pin (the only real unknowns)

1. **TRANSPOSE** (identity vs u↔v swap): Blender maps grid→ptex with a
   coordinate swap (`ptex_u=1-grid_v`), SculptCore defines `+u` = corner edge,
   `+v` = prev-corner edge. Whether these land on the same axis is a convention
   detail; resolve numerically.
2. **Corner rotational parity** within a face (§3): same direction or reversed.
3. **Crease/boundary rules** (watch item): Blender multires uses OpenSubdiv CC
   crease rules; SculptCore has its own `boundary::EDGE_SHARP` rules. At zero
   displacement the two discrete CC bases must agree to float tolerance on a
   *crease-free* cage; on creased cages they may diverge. The position-based
   round-trip still *functions* (it bakes whatever surface SculptCore produced),
   but detail shifts on creased meshes — document as a v1 limitation unless the
   test shows the bases agree.

---

## 5. Validation procedure (pins §4.1–§4.2, gates the whole workstream)

**Zero-displacement base-surface agreement.** Both engines are Catmull-Clark;
with empty displacement the subdivided *positions* must match.

1. Build a cube cage. Subdivide to level `L` in **both** systems with zero
   displacement (SculptCore: `Multires_new` + materialize level L; Blender: a
   multires modifier at `totlvl=L` with zeroed `CD_MDISPS`, eval mesh, converted
   to per-grid CCG order).
2. For every grid sample, compare `blender_abs[grid][y*side+x]` against
   `sc_abs[corr_grid[grid]][map(x,y)]` under each of the 4 candidate conventions
   (identity/transpose × forward/reversed corner order).
3. Exactly one convention should give agreement `< 1e-5` on a crease-free cube.
   That fixes TRANSPOSE and corner parity. Lock them into A3 as constants.
4. Repeat on: an n-gon cage (corner-count ≠ 4 → exercises the per-corner ptex
   path), a boundary-heavy cage (open mesh → `GridLink.grid == -1` seams), and a
   creased cage (quantify §4.3 divergence).

Deliverable of this step: the numbers + the chosen convention, appended here,
and the constants baked into the A3 helper.

### 5a. Numeric results (2026-07-17, `claudeMemory/scripts/p8_validate.py`)

Ran the cross-engine geometric comparison on a cube: SculptCore multires level-L
grid samples (via **A2** `Multires_levelPositionsOut`) vs Blender's Catmull-Clark
Subdivision-Surface eval at the same level, nearest-neighbour both directions.
**A2 verified** — sample counts exact: L1=96, L2=216, L3=600, L4=1944
(`6·4·(2^(L-1)+1)²`).

**Finding — the two base surfaces do NOT coincide at finite level.** They differ
by ~4× less each level (L1 0.199 → L2 0.0476 → L3 0.0118 → L4 0.0029), i.e.
O(4⁻ᴸ). A self-nesting discriminator (do a level's points lie on a finer level's
surface?) resolves why:

| | L2 points → L6 surface | verdict |
|---|---|---|
| SculptCore | 4.74e-2 | **discrete CC refinement** (control points sit off the finer surface) |
| Blender subsurf / multires | 0.000 | **CC limit surface** (limit points are level-independent) |

So **SculptCore's zero-displacement base is the discrete refined cage; Blender's
multires base is the CC limit surface** (matching plan §1: MDISPS is tangent to
the *limit*). They are not the same surface at any finite level.

**They converge to the SAME limit (no rule divergence on the cube).**
`SC-L2 → Blender-near-limit` (4.760e-2) ≈ `SC-L2 → SC-near-limit` (4.742e-2),
equal to 1.8e-4 — SculptCore's discrete surface and Blender's limit share one
limit surface; the discrete↔limit gap is the *only* difference. (The direct
`SC-L6 → Blender-L6` = 4.1e-3 is vertex-sampling offset at L6 density, not
divergence.)

**Impact — benign for the round-trip, by design.** Because §3 exchanges
*absolute* positions and each engine re-derives its own displacement against its
own base, the discrete-vs-limit gap is absorbed into the displacement channel on
each side; nothing needs to convert bases. The zero-displacement identity
round-trip still holds: Blender limit → import → SculptCore stores
`limit − discrete` as its top-level disp → export → SculptCore materializes
`discrete + (limit − discrete) = limit` → Blender bakes `limit − limit = 0`.

**Revises plan §5's premise** that "base-surface samples must agree to float
tolerance." They agree only in the limit. The robustness comes from the
absolute-position exchange (§3), not from base coincidence — so a base mismatch
is *expected*, not a convention error. The convention error the test *can* still
catch (crease-rule divergence, §4.3) would show as the two limits differing;
here they don't (cube has no creases — a creased-cage case is still owed).

**§4.1/§4.2 (transpose + corner parity) remain unpinned.** The geometric
point-cloud test is index-free and cannot pin them, and Blender exposes no
per-grid MDISPS/CCG positions to Python (confirmed — only the modifier settings
in `rna_modifier.cc`). Pin them at **Workstream B** via the round-trip oracle:
bake SculptCore's absolute top-level positions into MDISPS through
`multires_reshape_from_positions` under each of the 4 candidate conventions and
keep the one whose re-evaluated multires surface reproduces SculptCore's surface
(equivalently, whose baked MDISPS matches the intended displacement). B stands
up the reshape context anyway, so this adds no separate scaffolding.

---

## 6. API implied for Workstream A

- **A1** `Multires_fromLevelPositions(mesh::Mesh *cage, int maxLevel, const float (*positions)[3], const int *sample_to_vid, int sample_num)` — seed + writeback cascade. `sample_to_vid` is the A3 forward map (per-grid-sample → level-mesh vid).
- **A2** `int Multires_levelPositionsOut(Multires*, int level, float (*out)[3])` — materialize + dump absolute positions in CCG per-grid order; returns sample count. Free with `freeMeshBuffer` per the existing convention.
- **A3** grid map builder — from cage topology (Blender layout), emit `sample_to_vid` / `vid_to_sample` index arrays applying §2 with the §5-locked constants. Reuse `levelGridVertsOut` / `levelVertGridCoordsOut` rather than recomputing.
- **A4** (later) grid-mask channel `GridsStore::addChannel` 1-float ⇄ `CD_GRID_PAINT_MASK`, same A3 map.

Existing C-API to build on (`c-api/subdiv_c_api.cc`): `Multires_new`,
`_setActiveLevel`, `_activeMesh`, `_activeTree`, `_writeback`, `_downRefit`,
`_serializeStore`, `_restoreStore`.

---

## 7. Open questions carried forward

- §4.1/§4.2 constants — pending the §5 numeric test.
- §4.3 crease divergence magnitude — quantify; decide map-vs-document.
- Import per-grid extraction vs subdiv-vertex eval mesh: the CCG-context read
  (§3 Import step 1) avoids a second remap but needs a reshape context on enter;
  the eval-mesh path is simpler but lands in subdiv-vertex order (needs a
  Blender-subdiv-vertex → grid-sample map, which `assign_final_coords_from_vertcos`
  has internally but does not expose). Prefer the CCG read; fall back to eval
  mesh + a vertcos-order map only if the CCG context is awkward to stand up on
  enter.
