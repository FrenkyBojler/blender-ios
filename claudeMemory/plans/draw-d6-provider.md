# P5 D6 — Real external draw provider over SculptCore's SpatialTree

Status as of this session: **foundation done, engine + addon side remaining.**
The Blender-side seam (D1–D5) is complete and GUI-verified with a hardcoded-
triangle test provider. This doc specifies the remaining D6 work so it is
turnkey.

## De-risked (the critical unknown)

The provider callback runs on the main thread during draw sync and must be
**native C** (no Python/GIL per frame). The open question was whether the
per-GPU-node CPU buffers can be produced **headless** (the engine's Vulkan
backend is never initialized inside Blender). Answer: **yes.**

- `gpu::Buffer` (`extern/sculptcore/source/gpu/vbo.h:68`) is CPU-resident:
  `void *data`, `size`, `elemsize`, `update_buffer`/`update_start`/`update_end`
  dirty flags. The Vulkan backend uploads `data` only when present.
- `gpu::GPUManager` (`.../gpu/manager.h:28`) is a backend-agnostic frontend with
  a plain default constructor; `createBuffer` allocates a CPU-backed `Buffer`.
- `SpatialTree::gpu_nodes()` (`.../spatial/spatial.h:668`) returns the GPU-root
  nodes; `SpatialTree::update(GPUManager*)` (`:844`) fills each node's
  `GpuData.pos/nor` (de-indexed triangle soup, DFS `LeafSlice` order) via
  `fill_leaf_slice` / `regen_gpu_node`.

So the addon holds a headless `GPUManager` per session, calls `tree->update(gpu)`
after a stroke, and the provider reads `gpu_nodes()[i]->data->{pos,nor}->data`,
`total_verts`, node bounds, and the buffers' `update_buffer` flag.

## Already landed (Blender side)

- Provider ABI + gate + `ObjectModeType.draw_provider` slot (D1).
- Per-node GPU cache + `external_batches_get` (D2/D3), Workbench (D4), overlays
  (D5, incl. the outline prepass).
- ABI identifies the object by `ID.session_uid` (an `unsigned int` key the
  native provider maps to a tree), since native code can't deref an `Object*`.

## Remaining work

### R1 — RNA registration seam (Blender, `rna_object_mode.cc`)

The addon must hand Blender the native provider's 64-bit address. RNA function
int params are 32-bit, so carry it as a **string** class attribute
`bl_draw_provider` (decimal address), with `PROP_REGISTER_OPTIONAL` +
string get/length/set callbacks. The set callback parses `strtoull` and calls
`BKE_object_mode_draw_provider_set(mt, (const ExternalDrawProvider *)addr)`
(runs on the dummy during `validate`, then register memcpys `draw_provider`
into the real type). Addon usage: `SculptCoreMode.bl_draw_provider =
str(engine_provider_address)` before `register_class`.

### R2 — Engine c-api: external-draw ABI + provider (extern/sculptcore, submodule rebuild)

- Add a small versioned header mirroring `ExternalDrawNode` / `ExternalDrawProvider`
  (the ABI is duplicated across the repo boundary by design; guard with
  `BKE_EXTERNAL_DRAW_ABI_VERSION`).
- A native module exposing:
  - `sc_external_draw_register(unsigned int key, SpatialTree *tree)` /
    `_unregister(key)` — the key→tree registry the addon populates on
    enter/exit.
  - `const ExternalDrawProvider *sc_external_draw_provider()` — a static
    provider whose `nodes_get(key, req, r_nodes)` looks up the tree, walks
    `gpu_nodes()`, and fills a thread-local `Vector<ExternalDrawNode>` from each
    node's `GpuData` (pos/nor `->data`, `total_verts`, bounds from the node
    AABB, `update_flags` from `Spatial_RegenGPU`/`update_buffer`). `nodes_release`
    clears the scratch.
- Export the two entry points via the `LSTL_*`/`WASMSYM` list so the ctypes
  addon can read `sc_external_draw_provider`'s address.

### R3 — Addon wiring (scripts/addons_core/sculptcore_addon)

- On `register()`: after the engine loads, set
  `SculptCoreMode.bl_draw_provider = str(<provider address>)` (from the capi),
  then register the class.
- On `enter(ob)`: build the session as today, then
  `sc_external_draw_register(ob.session_uid, tree_ptr)` and create the session's
  headless `GPUManager` (`executor.meshLog`-style: a bound `gpu::GPUManager`).
- Stroke end / flush: `tree.update(gpu_manager)` so the GPU-node CPU buffers are
  current for the next redraw. (Replaces / complements the flush-to-Mesh once
  the provider path is proven; keep flush for save/render.)
- On `exit(ob)`: `sc_external_draw_unregister(ob.session_uid)`, dispose the
  GPUManager.
- Depsgraph/redraw: tag region redraw from the stroke operator (already done);
  no `ID_RECALC_GEOMETRY` per step needed on the provider path.

### R4 — Attributes + EEVEE (folds into P5 (b))

- Generic attribute request → `setTreeRequestedAttrs`; fill
  `ExternalDrawNode.attrs` from `GpuData.attrBufs`; add the vertex formats +
  `DRW_cdlayer_attr_aliases_add` aliases Blender-side (mask/face-set/color).
- `external_batches_per_material_get` + `eevee_sync` gating for EEVEE.

## Verification

- Sculpt a real mesh; the drawn viewport geometry updates per-node during the
  stroke (not a full re-extract), matching the flushed result on stroke end.
- Toggle detail/dyntopo; topology changes reflected (TOPOLOGY realloc path).
- Multi-object; `session_uid` routing draws each object's own tree.
- `--debug-gpu` leak check across mode enter/exit / provider unregister.
- Cycles viewport falls back to the evaluated mesh (gate already excludes
  external engines).
