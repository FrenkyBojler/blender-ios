# Master Task List — SculptCore Integration

Top-level checkbox tracker across all implementation plans for the SculptCore
integration (branch `sculptcore`). One section per plan; each links to the full
plan doc. Check items off as they land; keep phase order per the plan.

See [README.md](./README.md) for plan conventions and
[../README.md](../README.md) for the full memory index.

---

## Plans

| # | Plan | Status | Doc | Depends on |
|---|---|---|---|---|
| P1 | Python bindings | Not started | [python-bindings.md](./python-bindings.md) | — |
| P2 | Custom mode infrastructure (Blender C, Tier 1) | Not started | [mode-infra.md](./mode-infra.md) | — (parallel with P1) |
| P3 | Mesh ⇄ SculptCore conversion & lifecycle | Not started | [mesh-convert.md](./mesh-convert.md) | P1, P2 |
| P4 | Addon skeleton (v0 end-to-end slice) | Not started | [addon-skeleton.md](./addon-skeleton.md) | P1, P2, P3 (B1–B3), P7 (M1) |
| P5 | Draw integration (external draw provider) | Not started | [draw-integration.md](./draw-integration.md) | P2, P4 |
| P6 | Undo integration (wrapped undo + meshlog) | Not started | [undo-integration.md](./undo-integration.md) | P2, P3, P4 |
| P7 | Brush & settings mapping | Not started | [brush-mapping.md](./brush-mapping.md) | P1 (M1 feeds P4 early) |
| P8 | Multires conversion (MDISPS ⇄ grids) | Not started | [multires-convert.md](./multires-convert.md) | P3, P4 |

Research: [../research/sculpt-modifier-coupling.md](../research/sculpt-modifier-coupling.md)
maps where Blender couples sculpt structures to the modifier stack / geometry
nodes and records the v1 deferral decision (no sculpting with active
deform modifiers / geometry nodes / shape keys; multires supported by ignoring
the multires modifier). `../research/grid-correspondence.md` (to be written,
P8 P0) will pin the MDISPS ⇄ SculptCore grid-sample bijection.

**Critical path:** P2 → P3 → P4 (v0 usable) → P5/P6/P8 in parallel.
P1 gates P3/P4/P7 on the engine side. Strategy reminder: minimal
modifications to Blender's addon/registration surface — Blender C work is
confined to P2 (mode API), P5 (draw provider seam), P6 (wrapped undo type),
plus one small reshape utility in P8.

---

## P1 — Python bindings

Full plan → **[python-bindings.md](./python-bindings.md)**. Goal: a first-class
Python surface for `extern/sculptcore`, driven over the engine's **existing
`LSTL_*` `extern "C"` ABI** via `ctypes` (no CPython extension module), with
generated `.pyi` stubs for type checking. The Python runtime is a **port of
`source/litestl/binding/typescriptRuntime/`**; the stub emitter is a **C++
sibling of `generators/typescript.cc`**.

### Phase 0 — Prerequisites & decisions
- [x] Check out the `source/litestl` submodule — done; binding system, C++
      generators, and the `LSTL_*` ABI confirmed in-tree.
- [x] Decisions locked at recommended defaults: **D2** `ctypes`-only, **D3** 1:1
      camelCase naming, **D4** self-hosted stub gen — see plan §2/§7.

### Phase 1 — Workstream A: native shared library exposing `LSTL_*`
- [ ] Shared flavour of the litestl `binding` target that force-exports `WASMSYM`
      on native builds (Windows `/EXPORT:` list generated from `WASMSYM`).
- [ ] Aggregate `sculptcore_capi` shared lib (engine + binding) also exporting
      `initBindings` + `getBindingManager`.
- [ ] `make.mjs configure/build python` → `build/python/` → `libsculptcore_capi.*`.
- [ ] `LSTL_AbiVersion()` for the Python-side version guard.

### Phase 2 — Workstream B: Python `ctypes` runtime (port of `typescriptRuntime/`)
- [ ] `_capi.py` — loader + `LSTL_*` decls + `LSTL_GetBindingInfo` table (← `wasmInterface.ts`).
- [ ] `_descriptors.py` — read descriptor structs at `BindingInfo` offsets (← `binding.ts`).
- [ ] `_classgen.py` — dynamic class per struct; member get/set; dispatch (← `bind.ts`/`manager.ts`).
- [ ] `_marshal.py` — `void**` thunk marshalling + `LSTL_Method_Invoke`/`Constructor_Invoke` (← `setValue.ts`).
- [ ] `_bulk.py` — zero-copy `numpy` views + string/typed-array seams (← `boundVector.ts`/`vector.ts`/`string.ts`).
- [ ] `_lifetime.py` — ownership / dispose / `LSTL_Destructor_Invoke` / double-free guard.
- [ ] `_union.py` — union disambiguation via `LSTL_Union_RunDisPropFunc`.
- [ ] `__init__.py` — import-time `initBindings()`; public surface.

### Phase 3 — Workstream C: `.pyi` emitter (C++ sibling of `typescript.cc`)
- [ ] `generators/python.{cc,h}` — descriptor-driven `.pyi` (classes / typed
      members / `@overload` signatures / `IntEnum` / imports).
- [ ] `LSTL_GeneratePython` + `LSTL_FreePythonString` in `binding.cc` (mirror
      `LSTL_GenerateTypescript`); add to `WASMSYM`.
- [ ] `_gen.py` (`python -m sculptcore._gen`) — emit `.pyi` tree + `py.typed`;
      preserve hand-written stubs.

### Phase 4 — Workstream D: type-check + tests
- [ ] `pyrightconfig.json` (strict) + mypy-clean secondary bar.
- [ ] `test_smoke.py` — construct Mesh, run a dab; **no C module built**.
- [ ] `test_parity.py` — match the TS/WASM runtime on a shared scenario.
- [ ] CI: Pyright (gate) + mypy + `pytest`.

### Phase 5 — Workstream E: Blender addon packaging
- [ ] Ship shared lib + package + stubs as one importable unit; document load
      path + ABI-version check.
- [ ] Define the addon mode/undo → Python seam
      (→ [../design/addon-custom-modes.md](../design/addon-custom-modes.md)).

### Verification (per plan §5)
- [ ] ABI smoke passes with **no** C module built.
- [ ] TS/WASM-runtime parity holds.
- [ ] Pyright strict + mypy clean on stubs.
- [ ] `numpy` bulk view is genuinely zero-copy.
- [ ] Codegen is deterministic (byte-identical `.pyi` on re-run).
- [ ] Zero leaks after teardown (`LSTL_GetMemSize` / alloc tracker).

---

## P2 — Custom mode infrastructure (Blender C, Tier 1)

Full plan → **[mode-infra.md](./mode-infra.md)**. `bpy.types.ObjectModeType` +
`OB_MODE_CUSTOM`: the five hardwired blockers get generic branches; flush/
refresh seams wired. No undo type (P6), no draw hook (P5).

### Phase A — identity + registry + registrable type
- [ ] A1 `OB_MODE_CUSTOM` bit + `OB_MODE_ALL_MODE_DATA` (`DNA_object_enums.h`).
- [ ] A2 `Object.custom_mode_id[64]` (`DNA_object_types.h`).
- [ ] A3 C `ObjectModeType` + global registry (`BKE_object_modes.hh`, new).
- [ ] A4 `rna_object_mode.cc` (new, ← `rna_render.cc`): registrable type,
      `enter`/`exit`/`flush`/`refresh` trampolines, `have_function[]`.
- [ ] A5 Versioning/sanitize for unregistered idnames on load.

### Phase B — the five blocker branches
- [ ] B1 `mode_compat_test` custom branch (`bl_object_types` check).
- [ ] B2 `object_mode_op_string` + generic `OBJECT_OT_custom_mode_toggle`.
- [ ] B3 `ed_object_mode_generic_exit_ex` custom branch (forced exit safe).
- [ ] B4 `'CUSTOM'` enum item + dynamic `object_mode_set_itemf` items +
      `Object.custom_mode` + `space_view3d.py` header fix.
- [ ] B5 `CTX_MODE_CUSTOM` + dynamic `CTX_data_mode_string` (panels/tools key).

### Phase C — keymap, tools, flush
- [ ] C1 Dynamic keymap handler in `view3d_main_region_init` + hierarchy entry.
- [ ] C2 Tool-system keying via context string verified (expected ~no changes).
- [ ] C3 `flush` trampoline in `ED_editors_flush_edits_for_object_ex`.
- [ ] C4 `refresh` trampoline from `undo_post` (memfile-only Tier-1 undo).

### Verification (per plan §4)
- [ ] Test-addon callback-order matrix (enter/exit/switch/close/disable).
- [ ] Memfile undo + save flush contract holds; header/panels/keymap live.
- [ ] ASAN + `WITH_UNITY_BUILD=OFF` clean build.

---

## P3 — Mesh ⇄ SculptCore conversion & lifecycle

Full plan → **[mesh-convert.md](./mesh-convert.md)**. Bulk-array C-API on the
engine; Mesh ID stays authoritative via `flush()`; topology-unchanged fast
path. Deferral of modifier/GN/shape-key sculpting per
[../research/sculpt-modifier-coupling.md](../research/sculpt-modifier-coupling.md).

### Workstream A — SculptCore C-API
- [ ] A1 `Mesh_fromArrays` (Blender layout: positions/corner_verts/face_offsets).
- [ ] A2 `Mesh_toArrays` + old→new index map (freelist compaction aware).
- [ ] A3 Bulk attribute copy in/out (v1 layer set) using the index map.
- [ ] A4 `Mesh_topologyDirty` query (drives exit/flush fast path).

### Workstream B — Addon conversion module
- [ ] B1 `enter`: validate → gather → build engine mesh + tree → session.
- [ ] B2 `flush`: fast path (positions/layers) + slow path (topology rebuild,
      drop-with-warning for unconverted layers).
- [ ] B3 `exit`: flush + free.
- [ ] B4 `refresh`: rebuild from Mesh ID after foreign undo; generation bump.

### Verification (per plan §5)
- [ ] No-stroke round trip byte-identical over the mesh corpus (incl. n-gons,
      loose geometry, 1M verts).
- [ ] Fast-path flush ≤ tens of ms at 1M verts; enter ≤ few hundred ms.
- [ ] Dyntopo-stroke exit produces a valid Mesh with warned layer drops.
- [ ] ASAN over enter/stroke/exit/undo cycles.

---

## P4 — Addon skeleton

Full plan → **[addon-skeleton.md](./addon-skeleton.md)**. Package layout,
`SculptCoreMode(ObjectModeType)`, session registry, modal stroke operator,
keymap/tool/UI v0 — Tier-1 only (flush-to-Mesh draw, memfile undo).

- [ ] S1 Loadable skeleton: engine import + ABI guard; mode registers;
      enter/exit round-trips positions.
- [ ] S2 First stroke: DRAW brush end-to-end; Phase-0 flush throttle; memfile
      undo works; cursor overlay.
- [ ] S3 Usability: full keymap, single tool, brush panel, invert/smooth,
      pressure.
- [ ] S4 Lifecycle hardening: object/workspace switch, file open/close, addon
      disable mid-mode, refresh generations; ASAN session.
- [ ] S5 Integration points behind capability checks: draw provider (P5),
      wrapped undo (P6), multires (P8), brush breadth (P7).

---

## P5 — Draw integration

Full plan → **[draw-integration.md](./draw-integration.md)**. Phase 0 =
flush-to-Mesh (ships with P4 S2). Phase 1 = generic external-draw-provider
seam: provider describes CPU node arrays + dirty flags; Blender owns all GPU
objects (Vulkan `wrap_handle` gap rules out buffer sharing); Workbench +
EEVEE + overlays branch exactly where `use_pbvh_draw` branches.

- [ ] D1 Provider ABI + registry + `BKE_object_use_external_draw` gate.
- [ ] D2 `draw_external_geom.cc`: per-node batch cache, dirty-driven
      realloc/upload, attribute formats + aliases.
- [ ] D3 `external_batches_get(_per_material)` (`SculptBatch`-shaped).
- [ ] D4 Workbench, then EEVEE consumption branches.
- [ ] D5 Overlay engines gated (no double-draw).
- [ ] D6 Native provider over `SpatialTree` (`NodeFlags` → update flags;
      attribute requests → `setTreeRequestedAttrs`).
- [ ] Test provider (hardcoded nodes) validates D1–D4 before D6.

### Verification (per plan §5)
- [ ] Dirty-node-only uploads confirmed (RenderDoc); 1M+ tri interactive.
- [ ] Mask/face-set/color parity in Workbench; per-node materials in EEVEE.
- [ ] Cycles-viewport fallback to flushed mesh; no GPU leaks on exit/undo.

---

## P6 — Undo integration

Full plan → **[undo-integration.md](./undo-integration.md)**. One C
`CUSTOM_MODE` `UndoType` wrapping opaque addon state; addon state = meshlog
step id + size; C wrapper owns the entire memfile-interop protocol. History
coherence across memfile interleaves / mode-exit boundaries is the hard part
(plan §4 — three scenarios written as tests first).

- [ ] A1 `custom_mode_undo.cc`: encode/decode/free/foreach_ID_ref + memfile
      interop tags + boundary `use_memfile_step`.
- [ ] A2 `undo_encode`/`undo_decode`/`undo_free` trampolines +
      `undo_push_custom` runtime function.
- [ ] A3 Gate `refresh` to memfile decodes only.
- [ ] B1 Stroke bracketing + push on release (replaces `'UNDO'` memfile push).
- [ ] B2 `undo_decode` → meshlog undo/redo + draw dirty + flush-dirty;
      generation/token validation.
- [ ] B3 `undo_free` → `freeStep`.
- [ ] B4 Undo memory limit plumbing (`stepMemSize`/`setMaxUndoSteps` +
      truthful Blender `step_size`).
- [ ] §4 scenario tests: foreign-memfile interleave, exit boundary,
      multi-object/rename (`UndoRefID`).

### Verification (per plan §6)
- [ ] Delta undo/redo exact (positions + dyntopo topology).
- [ ] Interleave + boundary matrices pass; bounded memory over 200 strokes.
- [ ] Python exception in decode degrades safely; ASAN clean.

---

## P7 — Brush & settings mapping

Full plan → **[brush-mapping.md](./brush-mapping.md)**. Reuse Blender `Brush`
datablocks + per-Paint unified settings via `tool_settings.sculpt` (decision:
share the sculpt Paint slot — zero DNA change); declarative mapping table →
SculptCore's reflected `Brush` + kernel selection; engine-only uniforms become
auto-generated custom properties on `Brush.sculptcore` / `Scene.sculptcore`
(asset-serializable).

- [ ] M1 Mapping table + `apply_brush` (DRAW first; then per-type breadth:
      CLAY/SCRAPE/FILL plane family, GRAB, SNAKEHOOK, SMOOTH, INFLATE, PINCH,
      MASK, KELVINLET, POSE, SHARP, COLOR, ENHANCE, LAYERDRAW…).
- [ ] M2 Manifest walk → generated `PropertyGroup`s; idempotent register.
- [ ] M3 Brush UI panel (+ auto engine-props section, dyntopo panel).
- [ ] M4 Pressure → `pushDeviceInput` + by-name dynamics; autosmooth
      `[main, SMOOTH]` program; pixel-radius unprojection.
- [ ] M5 Per-brush-type parity smoke harness (headless).
- [ ] Phase 2: brush textures (`mtex` → `tex_*`), cavity automask.
- [ ] Parity checklist maintained for unmapped features (cloth/boundary/
      multiplane, topology rake, front-face, accumulate, tip shape).

---

## P8 — Multires conversion

Full plan → **[multires-convert.md](./multires-convert.md)**. Ignore the
multires modifier at runtime; convert via **absolute top-level positions**
(never frame-to-frame): MDISPS + limit surface → level positions → SculptCore
writeback cascade; export via reshape-context bake back into `CD_MDISPS`.

- [ ] P0 Grid-correspondence investigation → `../research/grid-correspondence.md`
      + mapping helper (zero-displacement CC agreement test; crease-rule
      divergence quantified).
- [ ] A1 `Multires_fromLevelPositions`; A2 `Multires_levelPositionsOut`.
- [ ] A3 Bijection C entry (Blender grid samples ⇄ level-mesh verts).
- [ ] A4 Grid paint-mask channel in/out.
- [ ] B Blender bake seam: `multires_reshape_from_positions` utility
      (preferred) or temp-object reshape path.
- [ ] C1 Enter: MDISPS → engine multires; suppress modifier viewport display.
- [ ] C2 Level UI (`sculptlvl` ⇄ `setActiveLevel`); subdivide/delete deferred.
- [ ] C3 Flush/exit: bake back to MDISPS.
- [ ] C4 Undo payload via `Multires_serializeStore`/`_restoreStore`
      External chunks (with P6).

### Verification (per plan §5)
- [ ] Zero-displacement and no-stroke identity round trips (incl. render
      comparison on a production multires asset).
- [ ] Edit round trip visible/correct in vanilla Blender at all view levels.
- [ ] Corpus: n-gon, creased, boundary-heavy cages; levels 1–6+.
