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
- [x] Shared flavour of the litestl `binding` target that force-exports `WASMSYM`
      on native builds — implemented as `lt_native_export_symbols()` in litestl
      `build_files/macros.cmake` (per-symbol `/EXPORT:` on Windows, `-u` /
      `--undefined` elsewhere), fed from the same `WASM_SYMBOLS` /
      `LT_WASM_SYMBOLS` global sets the WASM link exports (no drift possible).
      `webgpuRenderScene` registration gated on `BUILD_WASM` (WASM-only symbol).
- [x] Aggregate `sculptcore_capi` shared lib (engine + binding) also exporting
      `initBindings` + `getBindingManager` (root `CMakeLists.txt`, native
      branch; stages `wgpu_native.dll` beside it).
- [x] `make.mjs configure/build python` → `build/python/` → `sculptcore_capi.dll`
      (`libsculptcore_capi.so`/`.dylib` naming wired for other platforms).
- [x] `LSTL_AbiVersion()` for the Python-side version guard (= 1).
- Verified via plain-`ctypes` smoke: load, `LSTL_AbiVersion()==1`,
  `initBindings()`, `getBindingManager()`, `LSTL_Binding_GetKeys` → 139
  bindings. Note: build required fast-forwarding the litestl submodule to
  origin/master (`aabbRayEnter`); gitlink bump not committed yet.

### Phase 2 — Workstream B: Python `ctypes` runtime (port of `typescriptRuntime/`)
- [x] `_capi.py` — loader + `LSTL_*` decls + `LSTL_GetBindingInfo` table (← `wasmInterface.ts`).
      Native 64-bit reads via `from_address`; ABI-version guard; interned
      allocation-tag pool (litestl retains tag pointers).
- [x] `_descriptors.py` — read descriptor structs at `BindingInfo` offsets (← `binding.ts`).
      Field widths follow the C++ decls (Vector `size_` = size_t, string
      `size_` = int32, `EnumItem::value` = int32 regardless of baseSize).
- [x] `_classgen.py` — dynamic class per struct; member get/set; dispatch (← `bind.ts`/`manager.ts`).
      Overloads: last registration wins for now (signature dispatch TODO).
      List→Vector argument conversion deferred to the bulk slice.
- [x] `_marshal.py` — `void**` thunk marshalling + `LSTL_Method_Invoke`/`Constructor_Invoke` (← `setValue.ts`).
      Also fixes two TS-runtime gaps: enum args written by baseSize; scalar/
      pointer return buffers freed (struct-by-value returns become owning).
- [x] `_bulk.py` — `BoundVector` (live indexing, struct-element wrapping,
      `resize`, owning dispose) + zero-copy `numpy()` views (buffer-address
      identity verified) + `construct_from_items` (Python list → temporary
      `Vector<T>` args, disposed after the call). String-passing seams still
      pending (reads work via `read_litestl_string`; the c-api string entry
      points like `getStrData` are declared ad hoc when needed).
- [x] `_lifetime.py` scope — ownership / dispose / `LSTL_Destructor_Invoke` /
      double-free guard implemented on `BoundObject` in `_classgen.py`
      (context manager; explicit dispose, no GC finalizer). Split out only if
      it grows.
- [ ] `_union.py` — union disambiguation via `LSTL_Union_RunDisPropFunc`
      (descriptor side done in `_descriptors.UnionType`; dispatch pending).
- [x] `__init__.py` — `sculptcore.init()` loads + `initBindings()` + manager
      singleton (kept explicit rather than import-time so the lib path is
      overridable).
- Thin-slice verification: `python/tests/test_smoke.py` (6 tests) passes —
  enumeration, array/scalar member get/set, method invoke on a constructed
  `Mesh`, double-dispose guard, zero leak delta over 32 construct/dispose
  cycles. Pure ctypes; no C module built.

### Phase 3 — Workstream C: `.pyi` emitter (C++ sibling of `typescript.cc`)
- [x] `generators/python.{cc,h}` — descriptor-driven `.pyi` (classes / typed
      members / `@overload` groups for overloaded methods / `IntEnum` /
      sorted imports). Mapping: one flat module per C++ namespace
      (`sculptcore::mesh` → `sculptcore_mesh.pyi`); template instantiations
      become mangled concrete classes (members resolve ParentTemplateParam
      to concrete types — Python's answer to the TS mapped-type trick);
      `Vector<T>` → `BoundVector[T]`, `String` → `str`, `T*` → `T | None`;
      unions → `TypeAlias` named after `mapName`; embedded struct/array
      members emit as read-only `@property`.
- [x] `LSTL_GeneratePython` + `LSTL_FreePythonString` in `binding.cc` (mirror
      `LSTL_GenerateTypescript`); added to `WASMSYM` (exported natively via
      the same list).
- [x] `_gen.py` (`python -m sculptcore._gen`) — emits the `sculptcore/types/`
      stub tree + `py.typed` (+ package `py.typed`); only rewrites changed
      files; preserves hand-written files.
- Verified: 17 stub files; all parse (`ast.parse`); regeneration is
  byte-identical; **Pyright reports 0 errors** over the generated tree.

### Phase 4 — Workstream D: type-check + tests
- [x] `pyrightconfig.json` + mypy-clean secondary bar. Two gates:
      `pyrightconfig.stubs.json` (strict, generated stubs — 0 errors) and
      `pyrightconfig.json` (standard, runtime package + tests — 0 errors;
      dispatch rewritten to `isinstance` narrowing to get there).
      `python -m mypy --strict --follow-imports=silent sculptcore/types`
      is clean.
- [x] `test_smoke.py` — construct Mesh, member/method round trips, dispose
      guards, leak checks (6 tests) — **no C module built**. Plus
      `test_bulk.py` (vectors + zero-copy numpy, 3 tests) and `test_dab.py`:
      cube → spatial tree → reflected `CommandExecutor` `main` ctor →
      `filterNodes` → **DRAW dab moves geometry** (serializeMeshRaw diff).
      10 tests green. Note: `MeshLog.beginStep`/`endStep` are no longer
      reflected (napi_smoke.cjs predates a refactor) — dab runs unlogged;
      meshlog bracketing lands with P6 wiring.
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
- [x] A1 `OB_MODE_CUSTOM = (1 << 13)` + OR'd into `OB_MODE_ALL_MODE_DATA`
      (`DNA_object_enums.h`).
- [x] A2 `Object.custom_mode_id[64]` (`DNA_object_types.h`, after
      `restore_mode`; persists, sanitize-on-load pending in A5).
- [x] A3 C `ObjectModeType` + global registry (`BKE_object_modes.hh` +
      `intern/object_modes_custom.cc`, new): idname (dotted context string) +
      mangled `srna_idname` (RNA references, must persist), label/icon/
      object-type mask/keymap/flag, callback pointers, `py_instance`,
      `rna_ext`; add/remove/find/iterate/poll_object;
      `BKE_object_mode_types_exit()` wired next to `RE_engines_exit()`.
- [x] A4 `rna_object_mode.cc` (new, ← `rna_render.cc`): registrable
      `bpy.types.ObjectModeType` (`bl_idname`/`bl_label`/`bl_icon`/
      `bl_object_types` flag-enum over `1 << OB_*`/`bl_keymap`/
      `bl_use_custom_undo`), `enter`/`exit`/`flush`/`refresh` trampolines
      with `have_function[]`, persistent per-type py_instance released with
      `BPY_DECREF_RNA_INVALIDATE` at unregister. Verified headless:
      register / unregister / same-idname re-registration all pass.
      Gotchas hit: `PROP_ENUM_FLAG` must be set before items;
      `RNA_def_struct_ptr` references (doesn't copy) the identifier string.
- [x] A5 Load sanitize (`object_blend_read_data`): the custom bit never
      survives a *file* load (session state can't exist before addon
      registration; `custom_mode_id` persists as the restore target, and an
      addon may re-enter from a load-post handler). Undo reads keep the bit
      (the C4 refresh path re-syncs). Linked data already covered via
      `OB_MODE_ALL_MODE_DATA`. Verified: save-in-mode → reload → OBJECT with
      idname intact → `mode_set('CUSTOM')` re-enters.

### Phase B — the five blocker branches
- [x] B1 `mode_compat_test` custom branch: looks up the pending target (see
      B4) or `ob->custom_mode_id`, checks `bl_object_types` via
      `BKE_object_mode_type_poll_object`.
- [x] B2 `object_mode_op_string` branch + generic
      `OBJECT_OT_custom_mode_toggle` (`object_edit.cc`): optional `mode_id`
      string property; target resolution = property → pending → previous
      idname; `mode_compat_set` exits other modes; curves-toggle boilerplate
      (toolsystem update, DEG sync tag, msg-bus publish, ND_MODE notifier).
- [x] B3 `ed_object_mode_generic_exit_ex` custom branch (null-context exit
      trampoline; keeps `custom_mode_id` as restore info) +
      `custom_mode_exit_all()` used by RNA unregister to force-exit.
- [x] B4 `'CUSTOM'` static item in `rna_enum_object_mode_items` +
      `rna_enum_context_mode_items`; `object_mode_set_itemf` appends one item
      per registered mode (identifier = mangled `srna_idname`, 1-based
      registry index encoded in value high bits, decoded in
      `object_mode_set_exec` into the ED-level *pending target* — the enum
      value alone can't carry the idname); read-only `Object.custom_mode`;
      header shows the idname for 'CUSTOM'. Plain `mode_set(mode='CUSTOM')`
      re-enters the previous custom mode.
- [x] B5 `CTX_MODE_CUSTOM` slot + `data_mode_strings` "custom" fallback;
      `CTX_data_mode_string` returns the registered idname (panels/keymaps
      key off it). Note: `bpy.context.mode` is the context *enum* → 'CUSTOM';
      the dotted idname is the context *string* (bl_context matching).
- Verified headless (7 scenarios): toggle enter/exit with callbacks,
  `mode_set` 'CUSTOM'/'OBJECT'/dynamic-identifier/EDIT-switch-exits-first,
  and unregister-during-mode force-exit. Known cosmetic TODO: entering a
  custom mode logs a missing `builtin.select_box` tool warning (no tools
  registered for the mode yet — C2 territory).

### Phase C — keymap, tools, flush
- [x] C1 Dynamic keymap handler (`view3d_custom_mode_keymap_fn` in
      `space_view3d.cc`, tool-keymap pattern): resolves the active object's
      mode keymap per event; keymap ensured lazily in the default config,
      found through the user config. Deviations: no
      `keymap_hierarchy.py` entry (the hierarchy lists *fixed* names; addon
      keymaps registered via `keyconfigs.addon` appear in the UI already) and
      ensure-at-registration moved into the resolver (RNA register has no
      `wmWindowManager`). Event-level verification rides P4's addon keymap.
- [x] C2 Tool system: no changes needed — it keys off the context string
      (confirmed: entering a custom mode makes the toolsystem look up tools
      for the idname context and warn `builtin.select_box not found`, which
      goes away once the addon registers a tool for its mode, P4).
- [x] C3 `flush` trampoline branch in `ED_editors_flush_edits_for_object_ex`
      (before memfile undo encode / save / render). Deviation: no C-side
      dirty flag — dirty tracking is the addon's job (clean sessions return
      immediately); revisit via ObjectRuntime if profiling demands.
      Verified: flush fires on save while in mode.
- [x] C4 `refresh` trampoline in `ed_undo_step_post` (op_undo_depth-guarded,
      after UNDO/REDO_POST handlers; gating to memfile-only decodes is
      deferred to P6 A3 as planned). Verified interactively via remote_repl:
      real memfile undo fires exactly one `refresh` and the object stays in
      the mode across the Main swap.

### Verification (per plan §4)
- [x] Test-addon callback-order matrix: enter/exit via toggle + `mode_set`
      ('CUSTOM' / per-mode identifier / OBJECT), EDIT-switch exits first,
      unregister-during-mode force-exit, save/reload restore — headless
      scripts in the session scratchpad (promote into a shipped test with
      P4's addon skeleton).
- [x] Memfile undo + save flush contract: flush on save (headless) and
      refresh on real undo (interactive remote_repl) verified. Header enum
      renders (KeyError fixed by the static 'CUSTOM' item); panels/keymap key
      off `CTX_data_mode_string` — interactive panel/keymap-event check rides
      P4's addon (which registers real panels + keymap items).
- [ ] Object/workspace *GUI* switch matrix + addon-disable while in mode in a
      live session (generic-exit branch is unit-verified; GUI pass pending).
- [ ] ASAN + `WITH_UNITY_BUILD=OFF` clean build (run before the P2 work is
      called done-done; new headers + DNA touched).

---

## P3 — Mesh ⇄ SculptCore conversion & lifecycle

Full plan → **[mesh-convert.md](./mesh-convert.md)**. Bulk-array C-API on the
engine; Mesh ID stays authoritative via `flush()`; topology-unchanged fast
path. Deferral of modifier/GN/shape-key sculpting per
[../research/sculpt-modifier-coupling.md](../research/sculpt-modifier-coupling.md).

### Workstream A — SculptCore C-API
- [ ] A1 `Mesh_fromArrays` (Blender layout: positions/corner_verts/face_offsets).
- [ ] A2 `Mesh_toArrays` + old→new index map (freelist compaction aware).
- [~] A3 Bulk attribute copy — mask done: `Mesh_readVertFloatAttr` /
      `Mesh_writeVertFloatAttr` (named FLOAT vertex attr, live-vert order,
      create-on-write). Addon round-trips `.sculpt_mask` ⇄ `.spatial.v.mask`
      (loaded on enter so masking protects verts during sculpting; written
      on flush). Face sets done too: `Mesh_readFaceIntAttr` /
      `Mesh_writeFaceIntAttr`; addon round-trips `.sculpt_face_set` ⇄ the
      engine `group` face attr (loaded on enter, written on flush). The
      DRAW_FACE_SETS brush → POLYGROUP kernel, operator assigns a fresh
      `activeGroup` (maxFaceGroup + 1) per stroke. Verified for both mask
      and face sets: brush → flush creates the attribute, it persists across
      exit, re-enter reloads it unchanged. Color done too:
      `Mesh_readVertFloat4Attr` / `Mesh_writeVertFloat4Attr`; addon
      round-trips the active POINT/FLOAT_COLOR color attribute ⇄ the engine
      `color` float4 attr (PAINT→COLOR brush, `brushColor` from the Blender
      brush color; corner/byte color attrs left untouched with a warning).
      All three (mask/face-set/color) verified brush→flush→persist→reload.
      Autosmooth done: `build_autosmooth_program` builds a `[main, SMOOTH]`
      `BrushProgram`, the operator runs it via `execProgram` per dab when
      `brush.auto_smooth_factor > 0` (not grab-class / not SMOOTH). The
      SMOOTH strength is set by propId (`setCommandFloat(idx, 0, factor)`):
      the runtime cannot marshal a string into a `util::string` method arg
      (`litestl::util::String` has no constructors), the same constraint that
      makes `BrushFloatOverride` propId-keyed. Hardness fold-in done:
      `_bake_falloff` remaps the distance by `brush.hardness` before the
      falloff (inner `hardness` fraction reads full strength; `>= 1` is a
      hard disc), mirroring `apply_hardness_to_distances`. Verified: a hard
      SMOOTH spreads more than a soft one (falloff regression section).
      Still to add: UV, corner/byte color conversion, POSE (pose-segment
      placement — the engine's cage support is a partial "Wave 4b" slice),
      and the compaction index-map path.
- [ ] A4 `Mesh_topologyDirty` query (drives exit/flush fast path).

### Workstream B — Addon conversion module (`sculptcore_addon/convert.py`)
- [x] B1 `enter`: validate (refuse shape keys; warn on enabled modifiers /
      loose edges) → `foreach_get` positions/corner_verts/face_offsets →
      `Mesh_fromArrays` → `Mesh_buildSpatialTree` → session registry.
- [~] B2 `flush`: fast path done (positions via `dumpVertCo` +
      `foreach_set` + `mesh.update()`). Slow path (topology rebuild +
      layer drop-with-warning) still to write — raises a clear error for now
      (topology ops unreachable until dyntopo is wired). Layer round-trip is
      P3 A3 territory.
- [x] B3 `exit`: flush + free (re-entrant for forced exits).
- [x] B4 `refresh`: free + rebuild from the Mesh ID; generation bump.
      Verified via direct call (the C undo trampoline that invokes it is
      verified in P2).

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

- [x] S1 Loadable skeleton at `scripts/addons_core/sculptcore_addon/`:
      `engine.py` (single `sculptcore` import point — vendored `lib/` then
      `$SCULPTCORE_PYTHON_PATH`; ABI guard via `sculptcore.init()`; bulk
      c-api ctypes decls; session registry), `session.py` (engine
      mesh/tree handles, topology-stamp fast-path check, generation
      counter, re-entrant `free`), `convert.py` (P3 B1/B3/B4:
      enter=validate+`foreach_get`→`Mesh_fromArrays`+spatial tree;
      flush=positions fast path via `dumpVertCo`; exit=flush+free;
      refresh=rebuild+generation bump), `__init__.py`
      (`SculptCoreMode(ObjectModeType)`). Also fixed the viewport header's
      select/edit-menu fall-through for unknown mode strings (`'CUSTOM'`
      added to both exclusion sets). Verified headless (7 scenarios):
      enable → enter → no-stroke round trip byte-identical → engine edit
      flushes on save and on exit → refresh rebuilds with generation bump →
      addon-disable force-exits the live session.
- [~] S2 First stroke (DRAW): reusable dab core in `stroke.py`
      (`stroke_begin` → `apply_dab` = `filterNodes` + `execBrush` +
      throttled flush → `stroke_end` = `endStep` + `recalc_normals`) plus
      `raycast` (engine `castRay`) and the `SCULPTCORE_OT_brush_stroke`
      modal operator (`bl_options={'UNDO'}` → memfile bracket → mode flush).
      `mapping.py` M1 (DRAW: radius/strength/spacing/invert via `apply_brush`
      + `KERNEL_BY_TYPE`), `keymap.py` ("SculptCore Mode": LMB / Ctrl-LMB).
      Verified headless (raycast hits sphere pole → 50 DRAW dabs touch 50
      nodes → flush moves 44 units into the Blender mesh → exit persists) and
      interactively (same stroke; **memfile undo restores positions exactly,
      maxdiff 0.0**). Deferred: pixel-radius unprojection is written but only
      exercised in the GUI path; cursor overlay is S3; the modal operator's
      mouse-event path is smoke-checked (registers/polls) not driven.
      **Known P6 item:** memfile undo that crosses the mode-enter boundary
      lands back in Object mode leaving a stale `engine.sessions` entry (the
      exit-boundary hard case in undo-integration §4).
- [~] S3 Usability (partial): `tools.py` registers a `WorkSpaceTool`
      ("Brush") under the shared `'CUSTOM'` tool slot (both the C tool
      storage `CTX_MODE_CUSTOM` and the Python toolbar key off it); stroke
      input stays on the "SculptCore Mode" keymap so no tool keymap /
      double-fire. `ui.py` adds N-panel Brush + Dyntopo panels polled on the
      mode, reading the shared `tool_settings.sculpt`. Verified headless:
      tool lands in the CUSTOM slot, panels poll correctly (False in Object
      mode, True in mode), addon disable/re-enable is clean. Remaining:
      full keymap (smooth on Shift, radius/strength radials), invert
      already via Ctrl, pressure (M4), cursor overlay (GPU — GUI only).
      The former `builtin.select_box not found` warning is fixed: a custom
      mode now declares `bl_default_tool` (new `ObjectModeType.default_tool`
      field + RNA prop), and `toolsystem_reinit_ensure_toolref` resolves it
      for `CTX_MODE_CUSTOM` via `BKE_object_custom_mode_default_tool(ob)`
      before the generic fallback. `SculptCoreMode.bl_default_tool =
      "sculptcore.brush"`; verified the warning is gone on mode entry.
- [~] S4 Lifecycle hardening (partial): `handlers.py` reconciles the session
      registry against reality — `undo_post`/`redo_post` free any session
      whose object left the mode (fixes the memfile-undo-across-the-enter-
      boundary leak: the object drops to Object mode without an exit
      callback), and `load_post` drops every session (its engine meshes were
      built from the replaced file). Verified headless (register/unregister,
      reconcile keeps in-mode / frees deleted-object sessions, load_post
      clears all) and interactively (real undo across the boundary →
      `undo_post` fires → stale session gone, no leak). Object/workspace
      switch already exercised the generic-exit path (P2 B3); addon-disable
      force-exit verified (S1). Remaining: a live GUI switch matrix and an
      ASAN session.
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

- [~] M1 Mapping table + `apply_brush` — 10 brush entries verified working
      per-dab (`mapping.py`, keyed by *real* Blender `sculpt_brush_type`):
      DRAW→DRAW, DRAW_SHARP→SHARP, INFLATE, CLAY/CLAY_STRIPS→CLAY,
      PLANE→FILL, MULTIPLANE_SCRAPE→SCRAPE (plane family maps `plane_offset`),
      SMOOTH, PINCH (maps `pinch`←strength — the kernel gates on it),
      MASK (paints the mask attr, positions unchanged). `apply_brush` maps
      radius(world)/strength(+unified)/spacing(%→frac)/invert(dir⊕ctrl) then
      per-type extras + `writeProps`. **Crash guard:** GRAB/SNAKE_HOOK/POSE
      (need per-stroke anchor state — bare `execBrush` null-derefs) and LAYER
      (needs a sculpt-layer attr + texture) are in `UNSUPPORTED`;
      `kernel_enum` returns None so the stroke operator refuses cleanly.
      Grab-family: **fixed** the null-deref (missing `setStrokeGen` — gen 0
      collided with the fresh orig-gen default; now a nonzero per-stroke
      `session.stroke_gen` + `setGrabAccumAdd(False)` per dab). SNAKE_HOOK
      works via the standard per-dab path; GRAB works via a new grab-class
      path (`apply_grab_dab`: dab at the fixed anchor, `brush.grabTo`/
      `grabFrom` = cursor delta, node filter widened by the drag; operator
      projects the mouse onto the anchor plane). ELASTIC_DEFORM→KELVINLET
      also landed (grab-class, shares grabFrom/grabTo; engine mu/nu defaults).
      **14 brushes now sculpt** (incl. DRAW_FACE_SETS). Falloff mapping
      done: `apply_brush` bakes the Blender `curve_distance_falloff_preset`
      (SHARP/SMOOTH/SMOOTHER/ROOT/LIN/CONSTANT/SPHERE/POW4/INVSQUARE closed
      forms mirroring `BKE_brush_curve_strength`, CUSTOM samples the
      CurveMapping) into the engine's 256-entry `falloff_curve` LUT +
      `falloff_kind=Curve`. Verified: SHARP concentrates near center, SMOOTH
      between, CONSTANT spreads (regression suite `falloff presets`). Still
      to fill in: POSE (pose-cage), hardness fold-in, autosmooth
      `[main, SMOOTH]` program, PROJECTED falloff shape.
- [ ] M2 Manifest walk → generated `PropertyGroup`s; idempotent register.
- [ ] M3 Brush UI panel (+ auto engine-props section, dyntopo panel).
- [ ] M4 Pressure → `pushDeviceInput` + by-name dynamics; autosmooth
      `[main, SMOOTH]` program; pixel-radius unprojection.
- [~] M5 Per-brush-type parity harness (headless) — dabs each supported
      brush on a sphere and asserts the expected effect (DRAW/SHARP/INFLATE
      net-outward; CLAY/PLANE/SCRAPE/PINCH/SMOOTH move verts; MASK leaves
      positions put), plus a guard check that every `UNSUPPORTED` type is
      refused and `apply_brush` runs against real Blender brushes without a
      field-name error. All pass. (Verification script in the session
      scratchpad; formalize into a tracked addon test with S4.)
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
