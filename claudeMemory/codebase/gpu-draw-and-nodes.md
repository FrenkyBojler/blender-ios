# GPU / Draw Manager and Node Systems

Covers Blender's GPU backend abstraction, the Draw Manager and render
engines, and the node-tree / multi-function evaluation systems used by
geometry, shader, and compositor nodes.

---

## Part A — GPU Abstraction (`source/blender/gpu/`, prefix `GPU_`)

### Backend abstraction

- `class GPUBackend` — `source/blender/gpu/intern/gpu_backend.hh:41`. Pure
  virtual factory methods: `batch_alloc()` (65), `framebuffer_alloc()` (67),
  `shader_alloc()` (71), `context_alloc()` (63), etc. Every concrete backend
  (GL, Vulkan, Metal, headless "Dummy") implements this interface.
- Backend selection state: `g_backend_type` / `g_backend_type_override`
  (`intern/gpu_context.cc:362-364`), with
  `GPU_backend_type_selection_set()` / `_get()` (380 / 401) and
  auto-detection `GPU_backend_type_selection_detect()` (479).
- Instantiation: `gpu_context.cc:567,572,577,581` —
  `g_backend = MEM_new<GLBackend>(...)` /
  `MEM_new<VKBackend>(...)` /
  `MEM_new<MTLBackend>(...)` /
  `MEM_new<DummyBackend>(...)`, retrieved globally via `GPUBackend::get()` (637).
  The Vulkan branch is guarded by `#ifdef WITH_VULKAN_BACKEND` (~line 570).

### Key public types

Opaque C handles backed by internal C++ classes, one implementation per
backend under `source/blender/gpu/intern/gpu_*_private.hh`:

| Type | Private header |
|---|---|
| `Batch` | `gpu_batch_private.hh` |
| `FrameBuffer` | `gpu_framebuffer_private.hh:74` |
| `Texture` | `gpu_texture_private.hh:92` |
| `Shader`, `VertBuf` | corresponding `gpu_*_private.hh` |

### Vulkan backend location

`source/blender/gpu/vulkan/` — `vk_backend.cc/.hh` (`VKBackend : GPUBackend`),
`vk_context.*`, `vk_batch.*`, `vk_buffer.*`, `vk_descriptor_pools.*`, a
`render_graph/` subfolder implementing Vulkan's render-graph command
scheduling, plus `shaders/` and `tests/` subfolders. This is where recent
Vulkan work (see recent commit history: VKBatch resource uploads,
`assign_if_different` memory reuse, resource validation) lands.

---

## Part B — Draw Manager (`source/blender/draw/`, prefix `DRW_`)

- `DRW_render_to_image()` — declared `intern/DRW_render.hh:145`, defined
  `intern/draw_context.cc:1816`. Called by EEVEE
  (`engines/eevee/eevee_engine.cc:69`) and Workbench
  (`engines/workbench/workbench_engine.cc:802`) to run a full render pass.
- Engine registration: `DRW_engines_register()`
  (`intern/draw_context.cc:2286`) calls
  `RE_engines_register(&DRW_engine_viewport_eevee_type)` and the Workbench
  equivalent. Each engine's `RenderEngineType` struct is defined per-engine
  (e.g. `engines/eevee/eevee_engine.cc:79`). The companion
  `DRW_engines_free()` calls each engine's static `free_static()`
  (eevee, workbench, gpencil, image_engine, overlay, edit_select).
- The codebase is transitioning from the older C `DrawEngineType` struct to
  a C++ `Engine`/`DrawEngine` model — base type `struct DrawEngine`
  (`intern/DRW_render.hh:70`); per-engine implementations live under
  `source/blender/draw/engines/<name>/`.
- Global engine dispatch and per-frame draw execution machinery live in
  `intern/draw_manager.cc` / `draw_manager.hh`.

---

## Part C — Node Systems (`source/blender/nodes/`, prefix `NOD_`)

### DNA layer

| Struct | File:Line |
|---|---|
| `bNodeSocket` | `makesdna/DNA_node_types.h:1426` |
| `bNode` | `makesdna/DNA_node_types.h:1620` |
| `bNodeTree` | `makesdna/DNA_node_types.h:1869` |

### C++ runtime wrapper layer

- `class bNodeTreeRuntime : NonCopyable, NonMovable` —
  `source/blender/blenkernel/BKE_node_runtime.hh:119`. Caches derived state
  like `changed_flag`, `output_topology_hash`.
- `class bNodeRuntime` — same file, line 339.
- Related headers doing further C++-side tree analysis:
  `BKE_node_tree_interface.hh`, `BKE_node_tree_zones.hh` (repeat/simulation
  zones), `BKE_node_tree_update.hh`, `BKE_node_tree_reference_lifetimes.hh`
  (reference-lifetime analysis for geometry nodes).

### Node type registration

Pattern used throughout `source/blender/nodes/geometry/nodes/*.cc`: a local
`register_node()` function builds a `static bke::bNodeType ntype;`, sets
callbacks (`ntype.declare`, `ntype.geometry_node_execute`, `ntype.ui_name`,
…), then calls `bke::node_register_type(ntype)`. Concrete example:
`nodes/geometry/nodes/node_geo_transform_geometry.cc:108-119`.

`NOD_geometry.hh:14` also declares
`register_node_type_geo_custom_group(bke::bNodeType *ntype)` for the "custom
group" node type backing node-group instancing.

**There is no Python API for registering new evaluated geometry/shader node
types** — node *types* are C++-only, registered via `bke::node_register_type`.
Python-side RNA (`rna_nodetree.cc`) only exposes existing `NodeTree`/`Node`/
`NodeSocket` for scripting/UI (building node *groups*, not new node classes).
This is a notable contrast with the paint/tool system in
[python-bpy-integration.md](./python-bpy-integration.md), which is
Python-extensible.

### Multi-function / lazy-function system (`source/blender/functions/`, prefix `FN_`)

- `class MultiFunction : NonCopyable, NonMovable` — `FN_multi_function.hh:43`.
  A vectorized (SIMD-like batch) function abstraction with automatic
  multithreading/index-mask optimization; implemented across
  `intern/multi_function.cc`, `multi_function_procedure*.cc`,
  `multi_function_builder.cc`.
- `class LazyFunction` — `FN_lazy_function.hh:247`. `struct Context`
  (line 83; holds `void *storage` and `UserData *user_data`) is passed at
  execution time. Graph/executor types
  (`FN_lazy_function_graph.hh`, `FN_lazy_function_graph_executor.hh`,
  `intern/lazy_function_graph_executor.cc`) implement the demand-driven,
  multithreaded evaluation graph that geometry nodes compiles each
  `bNodeTree` into — each node becomes a `LazyFunction` node in a
  `lazy_function::Graph`. This is what lets geometry nodes evaluation pull
  only the outputs actually needed, and supports features like repeat zones
  and closures (e.g. `node_geo_evaluate_closure.cc`).

---

## Key File:Line Reference Table

| Topic | File | Line |
|---|---|---|
| `GPUBackend` interface | `gpu/intern/gpu_backend.hh` | 41 |
| Backend instantiation | `gpu/intern/gpu_context.cc` | 567-581 |
| `DRW_render_to_image` | `draw/intern/draw_context.cc` | 1816 |
| `DRW_engines_register` | `draw/intern/draw_context.cc` | 2286 |
| `DrawEngine` base | `draw/intern/DRW_render.hh` | 70 |
| Vulkan backend | `gpu/vulkan/vk_backend.cc` | — |
| `bNodeSocket` / `bNode` / `bNodeTree` | `makesdna/DNA_node_types.h` | 1426 / 1620 / 1869 |
| `bNodeTreeRuntime` / `bNodeRuntime` | `blenkernel/BKE_node_runtime.hh` | 119 / 339 |
| Example node registration | `nodes/geometry/nodes/node_geo_transform_geometry.cc` | 108-119 |
| `MultiFunction` | `functions/FN_multi_function.hh` | 43 |
| `LazyFunction` | `functions/FN_lazy_function.hh` | 247 |
