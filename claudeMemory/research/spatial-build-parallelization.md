# Spatial-tree build parallelization — investigation

Follow-up to the scale-benchmark finding (P3 verification): at 1M verts the
plain-Mesh `enter` costs ~3.2 s after the addon-side RNA wins, of which the
engine spatial-tree build (`Mesh_buildSpatialTree` → `SpatialTree::buildAll`)
is ~1.75 s.

## Phase profile (1M verts, RelWithDebInfo, SC_BUILD_PROFILE timing)

| phase | ms | parallel? |
|---|---:|---|
| clear_owner (reset `.spatial.{v,f}.node`) | 22 | trivial |
| calcAABB | 6 | trivial |
| recalc_normals | 90 | mesh method; parallelizable |
| shuffle (randomize insert order) | 6 | — |
| **add_face (incremental insertion)** | **1506** | **hard — the whole point** |
| ensure_node_tris ×1 | 73 | already parallel in the update path |
| regen_node_bounds ×1 | 45 | recursive; partly parallel |
| applyDeferredMerge (balance) | 0.6 | — |
| bounds/tris ×2 | ~0 | — |

**`add_face` is 86% of the build.** The easily-parallel phases (normals +
tris, ~163 ms, ~9%) cannot move the total meaningfully; the insertion is the
target.

## Why add_face is the cost, and why it is sequential

`buildAll` inserts faces one at a time (`add_face` → `add_face_intern`), each
descending from the root down a single centroid-chosen child and splitting a
leaf (`split_node`, a spatial-midplane partition) when it exceeds
`leaf_limit`. It is O(F · depth) and inherently serial: every face's path
depends on the current tree, and splits mutate shared structure. The random
shuffle exists only to keep the incrementally-grown tree balanced.

## Parallelization options

1. **Parallel top-down build (recommended).** Replace incremental insertion
   with a bulk recursive partition that already matches `split_node`'s
   spatial-midplane logic:
   - compute all face centroids in parallel;
   - `build(face_range, node)`: leaf when small enough, else pick the split
     plane from the node AABB, partition the range in place, create two
     children, and recurse — spawning a `litestl::task` per half;
   - assign ownership (`.f.node`/`unique_faces`) per leaf in parallel; resolve
     shared boundary `unique_verts` with an atomic claim (the code already has
     a `claimTag` mechanism for the parallel splits in `applyDeferredMerge`);
   - the existing parallel `ensure_node_tris` / `regen_node_bounds` finish it.
   Expected ~5-6× on this phase (→ ~0.3 s), so enter ~3.2 s → ~2.0 s.
2. **LBVH / Morton.** Morton-code + parallel radix sort + parallel tree build.
   Maximal parallelism but the furthest from the current spatial-midplane tree
   semantics; larger rewrite.
3. **Cheap phases only.** parallel_for the two `buildAll` tris loops and
   normals. Low-risk, proven pattern, but ~9% — does not move enter.

## Hazards a rewrite must respect

- **Node allocation under parallelism.** `alloc_node` grows `node_idmap`; the
  existing parallel-split path resolves candidates up-front because "nothing
  may read `node_idmap` once splits run in parallel." A top-down build
  allocates while descending → needs a pre-sized pool, a lock, or per-task
  allocation merged after.
- **Boundary `unique_verts`.** A vert shared by faces in different leaves must
  be claimed by exactly one; races on `treeMesh.v.node` → atomic CAS or a
  serial post-pass.
- **Determinism.** `buildAll` is deterministic (seed-0 shuffle); the reorder /
  locality-map paths and tests may rely on it. A parallel partition must stay
  deterministic (fixed split rule, stable partition) or the dependent code
  audited.
- Core spatial code — `documentation/spatial.md` invariants (GPU-node
  partition, ownership attrs) and the full spatial/brush/undo ctest suite must
  stay green.

## Status — DONE (parallel top-down build shipped)

`buildAllParallel` implemented (option 1) and made the default; the serial
build is kept as `buildAllSerial` (reference + `SC_SERIAL_BUILD` fallback).

Result at 1M verts: **buildTree 1749 → 346 ms (5.1x)**; the insertion phase
itself (`add_face` 1506 ms) became the `partition` phase at **32 ms (46x)**.
Enter overall 4.47 s → **1.86 s** across all the P3 perf work.

Final phase split (1M): prep 137 (mostly recalc_normals), centroids 14,
partition 32, renumber 2, vert_minface 14, vert_assign 86 (serial OrderedSet
populate — the next lever if needed), tris+bounds 61.

How the hazards were handled:
- **Vert ownership** — each vert owned by the leaf of its *lowest-indexed
  incident face* (atomic-min over faces, order-independent); that leaf always
  references the vert so its tri-AABB covers it (brush/raycast correct).
- **Leaf granularity** — the face threshold is scaled by the mesh F/V ratio so
  leaves hold ~`leaf_limit` *verts*, matching the serial build (a face-count
  threshold gave half-size leaves and broke the merge + reorder machinery).
- **Determinism** — a preorder-DFS renumber after the parallel partition gives
  deterministic node ids/`leaves()` order (parallel `alloc_node` alone is
  scheduling-dependent); face ownership is remapped through the id table.
- **Merge dropped** — the top-down partition is already balanced; running
  `applyDeferredMerge` over it over-merged and broke raycast, so the build
  ends after tris+bounds.

Validation: full engine ctest — the only failures (spatial_update_split,
bsmooth, the *_gpu tests, debug_script, live_stroke, multires) fail
identically on the serial build (pre-existing, GPU/backend config); the two my
change first broke (spatial_reorder_inc, spatial_displacement_bounds) now pass.
Addon 12-section regression green; scale-bench byte-identical at every size.

Remaining engine lever for enter: `Mesh_fromArrays` (~1.04 s at 1M) is now the
largest single cost.
