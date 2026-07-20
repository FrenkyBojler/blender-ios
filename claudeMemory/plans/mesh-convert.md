# Plan — Mesh ⇄ SculptCore Conversion & Mode Lifecycle

**Goal.** Fast, lossless conversion between `Mesh` and SculptCore's mesh on
mode enter/exit, plus the `flush()` implementation that keeps the Mesh ID
authoritative for save/undo/render mid-session. Multires grids are a separate
plan ([multires-convert.md](./multires-convert.md)); this plan covers the
base-mesh path.

**Dependencies.** [python-bindings.md](./python-bindings.md) (the addon drives
conversion through the bindings), [mode-infra.md](./mode-infra.md) (the
`enter`/`exit`/`flush` callbacks this implements). SculptCore-side C-API work
here can start immediately.

**Deferral note.** We do **not** support sculpting with active deform
modifiers / geometry nodes / shape keys — see
[../research/sculpt-modifier-coupling.md](../research/sculpt-modifier-coupling.md)
for the exact Blender coupling points we are skipping and why that is safe
indefinitely. The mode's poll/enter handles the presence of such modifiers as
a UX rule (refuse or warn), never as a data path.

---

## 1. Background (validated 2026-07-15)

**SculptCore mesh** (`extern/sculptcore/source/mesh/`): index-based
five-domain BMesh-like structure (`Mesh` at `mesh.h:48`; domains
VERTEX/EDGE/CORNER/LIST/FACE), paged attribute storage
(`AttrData<T>`, `ATTR_PAGESIZE = 4096`), n-gons supported
(dyntopo/draw triangulate internally). Build paths today:

- Euler ops `make_vertex`/`make_face` (`mesh.h:660-666`) — per-element, used
  by `mesh_shapes.cc:createCube`.
- The serializer's bulk `buildDomain` (`mesh_serialize.cc:622`) +
  `recountNgons()` (`mesh.h:113`) — bypasses per-element ops; the model for a
  new bulk import.
- Attributes: `Mesh::addAttr(domain, type, use)` (`mesh.h:270`), `AttrUse`
  COLOR/UV/POLYGROUP/SCULPT_LAYER; C API `getAttr`/`copyAttrRef`
  (`mesh_c_api.cc:25-194`).
- **Element indices leave freelist gaps after edits** — live ids are
  non-contiguous; iterate by capacity or compact before export.
- Winding: consistent, corner-order right-handed; no axis/unit assumptions —
  Blender coordinates pass through unchanged.

**Blender side:** with no deform modifiers, sculpt writes positions straight
into the original mesh (`PositionSource::Orig`, `pbvh.cc:894-920`) — our
conversion targets the same invariant: the Mesh ID is the persistent store.
Bulk Python paths are `foreach_get/set` and the attributes API (flat
float/int buffers, zero-copy compatible with the bindings' numpy views).

## 2. Design decisions

1. **Bulk array C-API on the SculptCore side, not per-element Python loops.**
   Add `Mesh_fromArrays` / `Mesh_toArrays` to `mesh_c_api.cc` taking Blender's
   native layout (positions `float3[verts_num]`, `corner_verts int[]`,
   `face_offsets int[]`) and building domains the way the serializer does.
   Python then does: `mesh.attributes` / `foreach_get` → numpy → one C call.
2. **The Mesh ID is authoritative** (design-doc invariant). `flush()` writes
   positions (+ changed attributes) back; everything persistent lives in the
   Mesh. SculptCore state that has no Mesh representation (e.g. its spatial
   tree) is rebuilt on `refresh()`/re-enter, never serialized into the file
   in v1.
3. **Attribute round-trip policy.** Convert positions always. The brush-target
   layers keep dedicated engine-name paths: `.sculpt_mask` (float, point) ⇄ the
   engine mask, `.sculpt_face_set` (int, face) ⇄ POLYGROUP, the active
   POINT/FLOAT_COLOR color ⇄ the engine `color` attr (name preserved via
   `session.color_attr_name`). **v2 — all other user attributes are carried
   through the engine** (`convert._load_bridged_attrs` → `Mesh_writeAttr` on
   enter): dyntopo interpolates every non-TOPO/NOINTERP layer and the meshlog's
   topology channel reverts them on undo, so the topology-rebuild flush recreates
   them (`_flush_bridged_attrs` → `Mesh_readAttr`) instead of dropping them.
   Covers vert/corner/face domains for the common types (float/float2/float3/
   float4, int/int2, bool, and byte/quaternion colors via the engine FLOAT4).
   Skipped: the engine edge domain (no stable Blender-edge correspondence — see
   §6) and every `.`-prefixed internal layer (topology/selection/mask). The
   fast (positions-only) path leaves Blender customdata untouched, so the bridge
   read-back runs on the rebuild path only.
4. **Topology-unchanged fast path.** If the session never ran a topology op
   (query SculptCore: dyntopo disabled and no topo meshlog chunks), exit =
   positions/attribute write-back only — no face rebuild, original indices
   valid (freelist untouched). This is the common case and must stay O(verts).

## 3. Change list

### Workstream A — SculptCore C-API (extern/sculptcore)

| # | Where | Change | Size |
|---|---|---|---|
| A1 | `source/mesh/c-api/mesh_c_api.cc` | `Mesh_fromArrays(positions, verts_num, corner_verts, corners_num, face_offsets, faces_num)` → build domains bulk (edges derived), `recountNgons`, return `Mesh*`. | M |
| A2 | same | `Mesh_toArrays`: compact-aware export — vert count, position dump (`gatherVertCos`/`dumpVertCo`, `mesh.h:506`), face walk (`f`/`l`/`c` columns) into `corner_verts`+`face_offsets`; returns an old→new vert index map when freelist gaps forced remapping. | M |
| A3 | same | Bulk attribute copy in/out for the v1 layer set: `(domain, type, name, void*)` in Blender domain order, using the index map from A2. | M |
| A4 | same | `Mesh_topologyDirty(mesh)` (or meshlog query): has any topology op run since a given stroke id — drives the fast path (decision 4). | S |

### Workstream B — Addon conversion module (Python, `sculptcore_addon/convert.py`)

| # | Change | Size |
|---|---|---|
| B1 | `enter(context, ob)`: validate (mesh object, no shape keys, warn/refuse on enabled non-multires modifiers per research note §3), gather `position`/`corner_verts`/`face_offsets` + v1 layers via `foreach_get`/attributes → `Mesh_fromArrays` + A3; build spatial tree (`Mesh_buildSpatialTree`); stash session (engine mesh, tree, executor, undo ids) keyed by object name + a session token. | M |
| B2 | `flush(ob)`: fast path — positions (+ dirty v1 layers) via A3/`foreach_set`, `mesh.update()` tagging; slow path (topology changed) — full A2 export → rebuild Mesh geometry (`mesh.clear_geometry()` + `from_pydata`-equivalent bulk attribute writes), re-add v1 layers, drop-with-warning others. Set normals dirty; `DEG_id_tag_update`. | M |
| B3 | `exit(context, ob)`: `flush` + free engine objects (lifetime module from python-bindings), clear session. | S |
| B4 | `refresh(context, ob)`: after foreign undo steps — rebuild engine mesh from the (possibly replaced) Mesh ID; invalidate meshlog redo (coordinate with [undo-integration.md](./undo-integration.md) §4). | M |

## 4. Order of work

1. A1/A2 with a SculptCore-side unit test (round-trip a cube + an n-gon mesh,
   assert winding + counts). Runs in sculptcore's own test harness, no
   Blender.
2. B1/B3 minimal (positions only) — enter/exit round-trips visually in
   Blender.
3. B2 flush fast path → memfile undo/save correctness (Tier-1 undo works).
4. A3 + layer round-trip; A4 + slow path; B4.

## 5. Verification

- Round-trip determinism: enter → exit with no strokes = byte-identical
  positions/topology/v1 layers (script over a corpus: quad mesh, tri mesh,
  n-gons, wire edges/loose verts, 1M-vert mesh).
- Loose geometry: SculptCore is face-oriented — confirm loose verts/edges
  survive (via A2 map) or are explicitly rejected at enter with a message.
- Fast-path timing: flush of a 1M-vert mesh ≤ tens of ms (positions memcpy +
  normals tag); enter ≤ a few hundred ms.
- Topology path: dyntopo stroke → exit → valid Mesh, warned about dropped
  layers, no dangling customdata.
- Save mid-session → reload → geometry matches viewport state (flush
  contract); ASAN pass over enter/stroke/exit/undo cycles.

## 6. Risks / open questions

- **Edge domain**: Blender meshes carry explicit edges (+ crease/bevel/seam
  attributes); A1 derives edges — seam/crease round-trip needs an edge-domain
  entry in the v1 layer policy once SculptCore's boundary constraints
  (`boundary::EDGE_SHARP`) are wired to Blender creases. Track under
  multires-convert (creases affect subdivision).
- **Vertex ordering after compaction** (A2 map) must flow through *every*
  layer copy and the undo coupling — single source of truth in the session.
- `mesh.clear_geometry()` on the slow path nukes all customdata; the v2 bridge
  (§3) recreates every carried user layer from the engine afterwards, so this is
  no longer lossy for vert/corner/face attributes. Edge-domain layers
  (creases/seams/sharp) remain dropped until an edge correspondence exists.
- Very large meshes: `foreach_get` into numpy is fine, but per-layer Python
  overhead adds up — keep the layer set small and measured.
