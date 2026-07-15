# Plan — Draw Integration (SculptCore Batches → Blender Viewport)

**Goal.** Get SculptCore's per-BVH-node geometry drawn by Blender's viewport
engines (Workbench, EEVEE, overlays) with per-node partial updates — the
performance property that makes sculpting dense meshes interactive. Two
phases: a zero-Blender-change fallback (flush-to-Mesh), then a generic
**external draw provider** seam mirroring the existing sculpt/PBVH draw path.

**Dependencies.** [mode-infra.md](./mode-infra.md) (the mode exists),
[mesh-convert.md](./mesh-convert.md) (flush path for Phase 0).
Phase 1 is the largest *new* Blender surface in the project — treat its
design section as a mini-design-doc.

---

## 1. Background (validated 2026-07-15)

### Blender's sculpt draw path (the model to mirror)

- Gate: `BKE_sculptsession_use_pbvh_draw` (`paint.cc:2788-2816`), consulted at
  `draw_context.cc:682` (excludes the object from the instanced handle-range
  path) and per-engine (`workbench_state.cc:298`, `eevee_sync.cc:276`,
  `overlay_*.hh`). Hardwired to `OB_MODE_SCULPT` + `SculptSession` +
  `bke::pbvh::Tree`.
- Batch cache: `draw_pbvh.cc` `DrawCacheImpl` (`:90-192`) — **one VBO per node
  per attribute** (pos `SFLOAT_32_32_32`, nor packed `SNORM_16_16_16_16`, mask
  `SFLOAT_32`, face-set `UNORM_8_8_8_8` precolored, generic attrs via
  `attribute_format` + aliases, `:280-330`), per-node IBOs (grids only; mesh &
  BMesh draw non-indexed), batches keyed by `ViewportRequest` (attribute set).
  Per-node-per-attribute dirty bits; `ensure_attribute_data` (`:1704`)
  recomputes only dirty ∪ missing, uploads only touched VBOs.
- Engine consumption: `draw_sculpt.cc` `sculpt_batches_get_ex` (`:51-133`) —
  frustum-culls nodes, returns `SculptBatch {batch, material_slot,
  debug_index}` per visible node. Workbench: `workbench_engine.cc:192,
  :351-395`; EEVEE: `eevee_sync.cc:289-324` (per-material via
  `DRW_mesh_get_attributes`). Per-node material = first face's
  `material_index` (`draw_pbvh.cc:1405`).
- **No existing injection point** for a foreign BVH: `sculpt_batches_get_ex`
  requires the real `SculptSession` + `bke::pbvh::Tree`. External-buffer
  adoption (`GPU_vertbuf_wrap_handle`) is unimplemented on Vulkan
  (`vk_vertex_buffer.cc:70-73`) — **GPU-buffer sharing is off the table**;
  CPU upload through `GPU_vertbuf_*` is the only cross-backend path.
- Python `gpu` draw handlers draw into the overlay framebuffer after the
  scene (`draw_context.cc:1333-1362`) — no EEVEE materials/lighting; not
  viable as the primary mesh display.

### SculptCore's draw data (already host-shaped)

- Per-GPU-node aggregated buffers (`GpuData`, `spatial/node.h:45-100`):
  `pos`/`nor` float3 streams + `attrBufs` — **CPU-resident**
  (`gpu::Buffer::data`, `vbo.h:79`), de-indexed triangle soup in DFS
  `LeafSlice` order, `total_verts` per node; tunable `gpu_tri_target`
  (default 2048 tris/node).
- Fill: `fill_leaf_slice` (`spatial_gpu.cc:60-157`), arbitrary attributes via
  `fill_leaf_attr` (`:165-231`); host attribute layout registered with
  `setTreeRequestedAttrs` / `refreshTreeRequestedAttrs`
  (`spatial_c_api.cc:57-102`).
- Partial updates: per-node `NodeFlags` (`Spatial_RegenGPU` topology,
  `Spatial_UpdateGPU` positions-only in-place slice rewrite, `spatial_gpu.cc:494`)
  + `Buffer::update_buffer` dirty flags after `SpatialTree::update()`.
- Triangle-soup non-indexed matches Blender's mesh/BMesh PBVH VBO layout
  exactly — conversion is a memcpy per dirty node (+ normal packing
  float3 → SNORM_16_16_16_16).

## 2. Phase 0 — flush-to-Mesh fallback (no Blender changes)

The addon's stroke loop periodically flushes positions to the Mesh
(fast path from [mesh-convert.md](./mesh-convert.md)) and tags
`ID_RECALC_GEOMETRY`. Full mesh re-eval + full batch re-extraction per update
— correct in every engine/overlay, prohibitively slow past ~100k verts, but
it makes the mode usable end-to-end while Phase 1 lands, and it remains the
permanent fallback for external render engines (Cycles viewport), which render
from evaluated meshes anyway (same rule as vanilla sculpt:
`external_engine` → no PBVH draw).

Throttle: flush at stroke-step granularity first; if too slow, flush on
timer + on stroke end. This phase is addon-only work (part of
[addon-skeleton.md](./addon-skeleton.md)).

## 3. Phase 1 — external draw provider seam

### Design

A generic, engine-agnostic C seam: an object in `OB_MODE_CUSTOM` whose mode
has a registered **draw provider** gets its geometry from provider-described
CPU arrays instead of the evaluated mesh. Blender owns *all* GPU objects
(VBOs/IBOs/batches — required anyway, see Vulkan wrap_handle gap); the
provider only describes geometry. SculptCore-specific code stays out of
Blender: the provider implementation lives in the addon's native lib and is
registered through a pointer-passing seam.

Provider ABI sketch (stable C structs, versioned):

```c
typedef struct ExternalDrawNode {
  const float (*positions)[3];   /* de-indexed triangle soup */
  const float (*normals)[3];
  const void **attrs;            /* per requested attribute, same order */
  int verts_num;                 /* multiple of 3 */
  int material_index;
  uint32_t update_flags;         /* TOPOLOGY (realloc) / DATA (re-upload) */
  float bounds_min[3], bounds_max[3];  /* for frustum culling */
} ExternalDrawNode;

typedef struct ExternalDrawProvider {
  int abi_version;
  /* Called once per redraw sync: attribute request in, node list out. */
  int  (*nodes_get)(void *user_data, const ExternalDrawAttrRequest *req,
                    ExternalDrawNode **r_nodes);
  void (*nodes_release)(void *user_data);
  void *user_data;
} ExternalDrawProvider;
```

The attribute request tells the provider which layers the engine needs
(the analogue of `ViewportRequest`), forwarded to SculptCore's
`setTreeRequestedAttrs`; the provider answers with all nodes + dirty flags,
Blender-side cache re-uploads only flagged nodes and rebuilds batches only on
attribute-set/topology changes (mirroring `DrawCacheImpl`).

Registration: `ObjectModeType` gains an optional provider slot; the addon
passes the provider struct pointer (created by its native lib) through a
Python-visible seam — an RNA function taking an int pointer
(`mode_type.draw_provider_set(ctypes-addressof)`), validated by
`abi_version`. Crude but contained; revisit if upstream review wants a
C-plugin-style handshake.

### Change list

| # | File | Change | Size |
|---|---|---|---|
| D1 | `blenkernel` (new `BKE_object_draw_provider.hh` or folded into `BKE_object_modes.hh`) | Provider struct + ABI version + register/lookup keyed off the mode registry; `BKE_object_use_external_draw(ob, rv3d)` gate (false for external engines, mirroring `use_pbvh_draw`). | S |
| D2 | `draw/intern/draw_external_geom.cc` (new, mirrors `draw_pbvh.cc` but much smaller) | Blender-side cache: per-node VBO/IBO-less batches per attribute-set, dirty-flag-driven realloc/upload (`GPU_vertbuf_create_with_format`, `GPU_batch_create(GPU_PRIM_TRIS, ...)`), normal packing, attribute formats + `DRW_cdlayer_attr_aliases_add` aliases so existing shaders bind. Cache stored per-object runtime, freed on mode exit / provider unregister. | L |
| D3 | `draw/intern/draw_sculpt.cc/hh` (or sibling `draw_external.hh`) | `external_batches_get(ob, features)` / `external_batches_per_material_get(ob, materials)` returning `SculptBatch`-shaped results (reuse the struct) with provider-side bounds-based culling. | M |
| D4 | `draw_context.cc:682` area; `workbench_state.cc` / `workbench_engine.cc`; `eevee_sync.cc` | Branch exactly where `use_pbvh_draw` branches: gate via D1, consume via D3. Workbench + EEVEE mesh sync only. | M |
| D5 | Overlays (`overlay_wireframe.hh`, `overlay_prepass.hh`, `overlay_facing.hh`, `overlay_fade.hh`, `overlay_mode_transfer.hh`) | Same gate so overlays don't double-draw the evaluated mesh; wireframe/lines support may be deferred (sculpt mode draws no edit wires). | M |
| D6 | Addon native side (extern/sculptcore or addon package) | Provider implementation over `SpatialTree`: `getDrawBatch`/GPU-node walk → `ExternalDrawNode` array; `NodeFlags`/`update_buffer` → `update_flags`; attribute request → `setTreeRequestedAttrs`. | M |

### Depsgraph/tagging

While the mode is active the object's evaluated mesh is untouched between
flushes, so no `ID_RECALC_GEOMETRY` per stroke step — redraws are driven by
region tagging (`ED_region_tag_redraw` from the stroke operator), same as
sculpt's PBVH path. Verify nothing else (shadows, EEVEE scene sync) requires
geometry tags per step; sculpt solves this today, copy its notifier pattern.

## 4. Order of work

1. Phase 0 (addon-side; ships with addon-skeleton).
2. D1 + D2 + D3 with a **test provider** (hardcoded triangle nodes in a dev
   operator) — proves the seam without SculptCore.
3. D4 Workbench first (simplest consumer), then EEVEE per-material.
4. D6 real provider; wire attribute requests (mask/face-set/color for
   Workbench feature parity).
5. D5 overlays; then measure + tune (`gpu_tri_target` vs node count, upload
   batching).

## 5. Verification

- Test-provider unit: batches render in Workbench/EEVEE; toggling dirty flags
  re-uploads only flagged nodes (RenderDoc / GPU debug groups).
- SculptCore stroke on 1M+ tri mesh: interactive frame rate during stroke;
  upload volume per frame ≈ dirty nodes only.
- Mask/face-set/color overlays match vanilla sculpt appearance in Workbench.
- EEVEE: multi-material mesh assigns per-node materials correctly; lighting
  matches the flushed result after stroke end.
- Cycles viewport (external engine): falls back to evaluated mesh (Phase 0
  behavior), no crash, geometry appears after flush.
- Mode exit / undo / file load with provider registered/unregistered: no
  leaked GPU resources (`--debug-gpu` leak check), no stale provider calls.

## 6. Risks / open questions

- **Upstream acceptability** of the provider seam is the project's biggest
  review risk — keep the ABI minimal, engine-agnostic, and justified by the
  addon-modes design doc; the alternative (addon fakes a `SculptSession` +
  `bke::pbvh::Tree`) couples us to sculpt internals far worse.
- Threading: engine sync may run while the stroke thread mutates nodes —
  define the handshake (provider snapshots under its own lock;
  `SpatialTree::update()` runs on the main thread before sync, like sculpt's
  batch update).
- Smooth shading: SculptCore currently fills flat normals in the whole-mesh
  batch path but per-vertex normals in the spatial path — confirm the spatial
  fill produces smooth normals; otherwise request vertex-normal fill mode.
- UV/tangent-needing EEVEE materials: `attrBufs` cover generic attributes,
  but tangents are computed by Blender's extractor — EEVEE materials needing
  tangents may render wrong until addressed; document as a v1 limitation.
