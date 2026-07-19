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
| P9 | Stroke quality & parity (reference-app insights) | Not started | [stroke-quality.md](./stroke-quality.md) | P4, P6, P7 (M1) |

Research: [../research/sculpt-modifier-coupling.md](../research/sculpt-modifier-coupling.md)
maps where Blender couples sculpt structures to the modifier stack / geometry
nodes and records the v1 deferral decision (no sculpting with active
deform modifiers / geometry nodes / shape keys; multires supported by ignoring
the multires modifier). `../research/grid-correspondence.md` (to be written,
P8 P0) will pin the MDISPS ⇄ SculptCore grid-sample bijection.

**Critical path:** P2 → P3 → P4 (v0 usable) → P5/P6/P8 in parallel.
P1 gates P3/P4/P7 on the engine side. P9 (addon-only) follows once
P4/P6/P7-M1 are in. Strategy reminder: minimal modifications to Blender's
addon/registration surface — Blender C work is confined to P2 (mode API),
P5 (draw provider seam), P6 (wrapped undo type), plus one small reshape
utility in P8; P9 adds none.

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
- [x] Object/workspace *GUI* switch matrix **done** (live event-sim session):
      (1) background-mode object while another is active (mode lock ON) is
      inert (stroke/cursor polls gate on the active object) and still draws
      via the provider; (2) outliner-activating a background in-mode object
      force-exits it through the generic path — vanilla lock-ON semantics
      (`outliner_select.cc` exits mode-incompatible objects), session freed
      cleanly; (3) activating the in-mode object itself keeps the mode;
      (4) switching to a workspace with a remembered mode (Modeling→EDIT)
      transitions cleanly through the compat path (nit: workspaces do not
      remember a custom mode); (5) addon disable while in mode force-exits,
      re-enable + re-enter work; (6) deleting an in-mode object leaked its
      session (only undo/load reconciled) — **fixed**: `handlers` now runs
      the reconcile on `depsgraph_update_post` too, so any real scene change
      sweeps stale sessions (verified live: the leak cleared on the next
      update).
- [x] ASAN + `WITH_UNITY_BUILD=OFF` clean build — **unity-off DONE**: full
      clang non-unity build (2187 targets, separate dir
      `../build_windows_x64_unityoff`) compiles and links with zero errors,
      and its binary passes the full multires session harness — no hidden
      include dependencies in any of the added headers. (Build note: run
      long builds DETACHED — killed tool-timeout rounds corrupt `.ninja_log`
      and restart the build from scratch each time.) ASAN refresh over the
      post-P6 C code: **DONE, all green** — six sync tests + the three p8 GUI
      harnesses (p8_session/p8_mask/p8_c4) run ASAN-clean (0 errors) with the
      P6 suppression setup. The re-run surfaced that six sync tests had gone
      VACUOUS: `execBrush` now runs `loadCommonProps` (pressure/M4), which
      overwrites engine-brush FIELDS from PROPS each dab — tests that set
      fields without `writeProps()` got strength/radius 0 (radius 0 → NaN
      positions, and `nan < threshold` comparisons never fail). Fixed by
      publishing via `writeProps()` (the bridge convention; the operator's
      `apply_brush` already does) and hardening the movement guards to
      `not (isfinite(...) and delta > eps)`. The meshchange failure was the
      same issue one level deeper: `resync_if_diverged` builds a fresh
      session brush, so props must be re-published after a rebuild.
      `bpy_class_call` (bpy_rna.cc) got the same dev-only
      `no_sanitize_address` exclusion as `bpy_class_validate_recursive`
      (identical `co_argcount` obmalloc false positive, hit via header_draw)
      so the p8 GUI harnesses can run under ASAN.

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
- [x] B2 `flush`: fast path (positions via `dumpVertCo` + `foreach_set`) +
      slow path — on topology change, `Mesh_toArrays` → `mesh.clear_geometry`
      + `from_pydata` rebuild; session sizes/stamp resynced so the next flush
      is fast again; the v1 attribute layers (mask/face-set/color) re-flush
      onto the new topology (other customdata dropped, matching vanilla
      dyntopo). Verified via the dyntopo stroke (114→1358 verts rebuilt to
      match). Bulk `foreach_set` rebuild (vert/loop/poly domains from the flat
      arrays + `update(calc_edges=True)`) — ~22k verts in ~30 ms (vs the
      O(faces) `from_pydata`); `validate()` clean. Also fixed a real bug: the
      positions fast path scattered by *engine index*, which after dyntopo has
      freelist gaps exceeding `verts_num` (out-of-bounds); it now writes in
      live-iteration order (the order `dumpVertCo`/`Mesh_toArrays` share), so
      the i-th row is Blender vert i. Regression covers a post-dyntopo
      fast-path flush on the gappy mesh.
- [x] B3 `exit`: flush + free (re-entrant for forced exits).
- [x] B4 `refresh`: free + rebuild from the Mesh ID; generation bump.
      Verified via direct call (the C undo trampoline that invokes it is
      verified in P2).

### Verification (per plan §5)
- [x] No-stroke round trip byte-identical over the mesh corpus — DONE, gated
      by `claudeMemory/tests/scale_bench.py`: a topology corpus (quad grid,
      triangulated grid, n-gon fan) plus every benchmark size up to 1M verts
      round-trips enter→flush **byte-identical** (`np.array_equal`, maxdiff
      0). Loose-edge-only meshes are still owed (validate warns but the round
      trip is unverified for them).
- [~] Fast-path flush ≤ tens of ms at 1M verts; enter ≤ few hundred ms —
      **measured, targets NOT met, but the shape is reassuring**
      (`scale_bench.py`, RelWithDebInfo engine DLL). Every phase scales
      **linearly** 50k→1M (gather/fromArrays/loadAttrs/draw ~18-20×,
      buildTree ~27×) — **no hidden O(n²)**, so the architecture is sound and
      it is a constant-factor problem. At ~1M verts: enter **4.5 s** (target
      ~400 ms), fast flush **197 ms** (target ~50 ms), per-DRAW-dab ~42 ms.
      Enter splits roughly evenly across gather (0.73 s, Blender RNA
      `foreach_get`) / fromArrays (0.89 s) / buildTree (1.78 s) / loadAttrs
      (0.97 s, dominated by the 4M-corner UV `foreach_get`) / draw (0.10 s).
      Ruled out: flat-grid BVH degeneracy (a domed 200k mesh builds in the
      same 353 ms as flat). **Prime suspect:** the litestl leak-tracking
      allocator is compiled into the native/python DLL for *every* build type
      (`NO_DEBUG_ALLOC` is only set for WASM+ASAN) — every alloc/free takes a
      global mutex and links onto a per-thread list, a per-allocation tax that
      would be off in a shipping build. Next step to quantify: rebuild the
      python target with `-DNO_DEBUG_ALLOC` and re-measure; then the
      Blender-side RNA `foreach_get` costs (gather + UV load) are the
      addon-side lever.
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
      **Amended (stroke-quality pass):** (1) dab **spacing** implemented —
      StrokeSpacer port (engine `brush/stroke_spacing.h` semantics) driven in
      **2D screen space** (interval = pixel radius × spacing fraction), each
      spaced point projected onto the surface; 3D-hit-polyline spacing was
      tried first and rejected (dab density coupled to the surface deforming
      under the stroke — 78 % A/B divergence between event rates, vs ~one-dab
      jitter with 2D spacing). (2) **Deferred write-back** — dabs and stroke
      release only refresh the draw provider; the Mesh ID syncs on demand via
      the mode flush callback (memfile encode / save / render / exit). The
      divergence guard now compares `session.blender_verts_num` (Blender count
      at last sync), not the live engine count, so an unflushed dyntopo stroke
      does not trip a session rebuild. Gates:
      `claudeMemory/tests/deferred_flush_test.py` (deferred contract incl.
      save/exit sync + guard) and a GUI pass driven via
      `--enable-event-simulate` (real LMB stroke through keymap → operator;
      40-move vs 8-move strokes near-identical). Full P6 + P8 suites green.
- [~] S3 Usability (partial): `tools.py` registers a `WorkSpaceTool`
      ("Brush") under the shared `'CUSTOM'` tool slot (both the C tool
      storage `CTX_MODE_CUSTOM` and the Python toolbar key off it); stroke
      input stays on the "SculptCore Mode" keymap so no tool keymap /
      double-fire. `ui.py` adds N-panel Brush + Dyntopo panels polled on the
      mode, reading the shared `tool_settings.sculpt`. Verified headless:
      tool lands in the CUSTOM slot, panels poll correctly (False in Object
      mode, True in mode), addon disable/re-enable is clean.
      **Cursor overlay done**: `ObjectModeType` gained an optional
      `draw_cursor(context, x, y)` callback dispatched through the WM
      paint-cursor mechanism (the only path that redraws the region on bare
      mouse moves) — one activation per registered type in
      `rna_object_mode.cc` (guarded for no-wm registration, freed at
      unregister); the trampoline passes region-local pixel coords with
      pixel-space GPU matrices pushed around the call. `cursor.py` draws the
      (unified) pixel-radius circle in the brush's cursor color; a failing
      draw reports once and disables itself. GUI-verified via event
      simulation: circle at the mouse, tracks moves, strokes run with it
      active, gone outside the mode.
      **Keymap pass done**: the stroke operator gained a `mode` enum
      (NORMAL/INVERT/SMOOTH, vanilla-style); Ctrl-LMB → INVERT (live Ctrl
      still toggles mid-stroke), Shift-LMB → SMOOTH (engine SMOOTH kernel at
      the active brush's radius/strength; no grab/dyntopo/autosmooth in a
      smooth stroke). F / Shift-F bind the standard `wm.radial_control` to
      the shared sculpt Paint's size/strength with the vanilla unified-aware
      property paths. GUI-verified via event simulation: a DRAW bump (0.39
      radial deviation) smoothed to 0.04 by three Shift-strokes; F radial
      shows the standard overlay and landed 100→380 in the unified size.
      Pressure landed with M4 (see P7). S3 complete.
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

- [x] D1 Provider ABI (`BKE_object_draw_provider.hh`) + registry
      (`ObjectModeType.draw_provider` + `BKE_object_mode_draw_provider_set`) +
      `BKE_object_use_external_draw` gate.
- [x] D2 `draw_external.cc`: per-node GPU cache (pos VBO + packed-normal VBO +
      TRIS batch), dirty-driven realloc/upload. Generic attribute formats +
      aliases still to come (positions + normals only so far).
- [x] D3 `external_batches_get` + `external_batches_per_material_get`
      (`SculptBatch`-shaped, frustum-culled).
- [~] R4 Generic attributes in the fast draw path. **Color + UV done**: the
      provider exposes each GPU node's per-attribute buffers, and `draw_external`
      draws color in Workbench vertex-color shading and UV in texture shading via
      `init_format_for_attribute` + `DRW_cdlayer_attr_aliases_add` (GUI-verified:
      a red/blue point color, and a COLOR_GRID checker sampled through the active
      UV map on a sculptcore-mode sphere). UV routes through the engine's dynamic
      per-attribute layout (`sc_external_draw_enable_dynamic` → `setRequestedAttrs`
      color@0 + uv@1, plus a linked stub draw shader); the addon seeds the engine
      `uv` corner attribute on enter (`Mesh_writeCornerFloat2Attr`).
      **Cross-pass batch fix**: the built attribute set is derived from what the
      object *has*, not the caller's `SculptBatchFeature`, so passes that request
      the same object in one frame with different flags (workbench / overlay
      outline / EEVEE per-material) share one stable per-node batch instead of
      reallocating it in place and freeing vertex buffers a returned batch still
      referenced (a use-after-free that surfaced as a null vtable call in
      `VKBatch::ensure_data_uploaded`). Remaining (lower priority): EEVEE material
      attrs beyond UV via the same dynamic path. See
      [draw-d6-provider.md](./draw-d6-provider.md) R4.
- [x] D4 engine consume: **Workbench** (gate + dispatch + batch source +
      instanced-path exclusion in `draw_context.cc`) and **EEVEE**
      (`sync_sculpt` external branch + `external_batches_per_material_get`), both
      GUI-verified (test triangle, then a real sculpted sphere in Solid and
      EEVEE Material Preview). Three GUI-only crashes fixed en route: null-PBVH
      sculpt handle, STATIC-usage VBO use-after-free, NodeCache move double-free.
- [x] D5 Overlay engines gated: Prepass/Facing/Fade/Mode-transfer draw the
      provider geometry (outline follows it, not the mesh); Wireframe skips
      external-draw objects (provider wireframe deferred).
- [x] D6 Native provider over `SpatialTree` — **done and GUI-verified** (a
      sculpted sphere renders per-node through the engine, not flush-to-Mesh).
      Engine c-api (`spatial/c-api/external_draw.{h,cc}`: key→tree registry +
      provider walking `gpu_nodes()` + headless `GPUManager` fill), RNA
      `bl_draw_provider` seam (64-bit address as a string), addon wiring
      (register/update/unregister by `ID.session_uid`). Design +
      de-risking in **[draw-d6-provider.md](./draw-d6-provider.md)**. Generic
      attributes (mask/face-set/color) in the draw + EEVEE are R4 (P5 (b)).
- [x] D6 teardown fix — the per-object GPU cache (`object_caches()`, a
      function-local static Map in `draw_external.cc`) was only ever freed by its
      C-runtime atexit destructor, which runs *after* the GPU backend is gone in
      `WM_exit`; freeing a cached vertex buffer then locked a destroyed Vulkan
      resource pool (a null-mutex crash on exit, in `VKDiscardPool::discard_buffer`,
      once any sculptcore object had been drawn). Now released from
      `DRW_module_exit()` (runs in `RE_engines_exit()` with a live GPU context,
      before `GPU_exit()`). Verified under cdb: crash gone.
- [x] Test provider: `OBJECT_OT_external_draw_test_toggle` (dev-only, hardcoded
      triangle) validates D1–D4. Verified headless to the draw-call boundary
      (`p5_verify` toggles the mode + satisfies the gate); the pixel render needs
      a GUI GPU context (Windows background has none) — manual check pending.

### Verification (per plan §5)
- [~] Dirty-node-only uploads confirmed (RenderDoc); 1M+ tri interactive.
      Headless dab cost measured at 1M verts (`scale_bench.py`): ~42 ms per
      DRAW dab (whole-mesh region, no spacing) — borderline-interactive on
      the CPU dab alone, before draw. RenderDoc dirty-upload confirmation and
      the live GPU frame time still need a GUI GPU context.
- [ ] Mask/face-set/color parity in Workbench; per-node materials in EEVEE.
- [ ] Cycles-viewport fallback to flushed mesh; no GPU leaks on exit/undo.

---

## P6 — Undo integration

Full plan → **[undo-integration.md](./undo-integration.md)**. One C
`CUSTOM_MODE` `UndoType` wrapping opaque addon state; addon state = meshlog
step id + size; C wrapper owns the entire memfile-interop protocol. History
coherence across memfile interleaves / mode-exit boundaries is the hard part
(plan §4 — three scenarios written as tests first).

- [x] A1 `custom_mode_undo.cc`: encode/decode/free/foreach_ID_ref +
      `UndoRefID` object tracking. (Design change from the plan: the step stores
      an integer `state_id` + truthful `size` passed through the push operator,
      not an opaque `PyObject *` from an `undo_encode` trampoline — no GIL work
      at free time. `UNDOTYPE_FLAG_DECODE_ACTIVE_STEP` so the type reverts its
      own step on undo rather than relying on the destination memfile decode.)
      **Amended in P8 C4:** `ut->poll = nullptr` (like SCULPT) — steps come
      only from the explicit typed push; generic pushes in-mode (property
      edits) fall through to memfile so DNA changes stay undoable. Paired
      with a flush-on-final-decode in addon `undo.decode` (the correct-order
      memfile restore below a custom step replaces Mesh data the engine must
      re-assert). Full P6 suite re-verified after the change.
- [x] A2 `undo_decode`(+`is_final`)/`undo_free` trampolines +
      `OBJECT_OT_custom_mode_undo_push` (replaces the planned
      `undo_encode`/`undo_push_custom`).
- [x] A3 Gate `refresh` to skip modes that provide custom undo (their
      `undo_decode` resyncs; a rebuild would discard the meshlog).
- [x] B1 Stroke bracketing + push on release (dropped `'UNDO'`; `undo.push`).
- [x] B2 `undo_decode` → meshlog seek (cursor-tracked, `is_final`-aware) + draw
      dirty + flush; generation validation. `convert.flush` now also detects a
      topology revert by live-vs-Blender vertex count (undo doesn't roll the
      topo stamp back).
- [x] B3 `undo_free` → `freeStep`.
- [x] B4 Undo memory limit plumbing: truthful Blender `step_size` (from
      `stepMemSize`) + `ED_custom_mode_undo_push` applies the same step-count and
      memory limits as `ED_undo_push`, so Blender's limiter evicts old custom
      steps and `undo_free`→`freeStep` reclaims meshlog memory. Verified
      (`custom_undo_memory_test.py`): 6MB pushed → ~2MB retained at a 2MB limit.
- [x] §4 scenarios verified:
      - Single-object memfile-boundary crossing (engine reverts on undo,
        self-corrects on redo) — `custom_undo_b_test.py`.
      - Foreign mesh-preserving memfile interleave (scene tweak mid-sculpt):
        undo x4 / redo x4 coherent and exact — `custom_undo_interleave_test.py`.
      - Multi-object routing via `object_ref`: two live sessions, undo/redo hit
        the right object — `custom_undo_multiobj_test.py`.
      - Foreign mesh-*changing* step: `convert.resync_if_diverged` (vertex-count
        guard at stroke start) rebuilds the stale session, bumping generation so
        orphaned steps no-op — `custom_undo_meshchange_test.py`.
      - Exit boundary: no crash undoing across it — `custom_undo_robustness_test.py`.
      Accepted v1 coarseness (documented): per-stroke redo into a session a
      foreign step rebuilt degrades to memfile-level; position-only foreign
      edits are not detected by the vertex-count guard.

### Verification (per plan §6)
- [x] Delta undo/redo exact — positions (`custom_undo_b_test.py`) and dyntopo
      topology (`custom_undo_dyntopo_test.py`); save round-trip preserved.
- [x] Bounded memory over 120 strokes at a small undo limit
      (`custom_undo_memory_test.py`).
- [x] Interleave + boundary matrices (foreign-memfile mesh-preserving +
      mesh-changing, multi-object, exit boundary) — see §4 above.
- [x] Python exception in decode degrades safely; undo across the mode-exit
      boundary does not crash (`custom_undo_robustness_test.py`).
- [x] ASAN: all eight custom-undo tests pass **ASAN-clean (0 errors) with
      user-poisoning enabled** on an MSVC + `WITH_COMPILER_ASAN` build (the
      clang-cl preset can't link the ASAN runtime; only MSVC's
      `/fsanitize=address` is wired in `platform_win32.cmake`). This covers the
      memory-relevant paths — push/decode/free, eviction, session free/rebuild,
      exit boundary, dyntopo topo replay, multi-object. Getting there needed two
      dev-only workarounds for a PRE-EXISTING false positive (the ASAN-built
      bundled CPython poisons its own obmalloc pools; Blender reads PyObject
      fields from them during class registration — reproducible with a trivial
      operator): a runtime suppressions file (`interceptor_via_lib:python313.dll`,
      `claudeMemory/scripts/asan_suppressions.txt`) for the mem-interceptor reads
      + a `__declspec(no_sanitize_address)` on `bpy_class_validate_recursive`
      (MSVC has no file-based ignorelist). Both are CLAUDENOTE-marked / guarded
      and revert before the PR. The full addon regression under ASAN trips more
      of the same CPython-poison false positives in unrelated `bpy.props`
      registration paths (out of scope); the real root fix is a non-poisoning
      Python (`PYTHONMALLOC`, which the bundled ASAN Python ignored here). See
      [[blender-asan-windows]].

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
- [x] M2 **done** — `engine_props.py`: at register a throwaway engine
      mesh/tree/brush/executor walks `queryUniformManifest` per mapped
      kernel; every engine-only float uniform (not mapping-driven, with a
      bound Brush field — the plain dab path reads fields) becomes a
      `FloatProperty` on the generated `Brush.sculptcore` PropertyGroup
      (asset-serializable; values survive addon re-enable as IDProperties).
      Range from the manifest `@range`; **default from the engine FIELD, not
      the DSL `def`** (planeSide is +1 as a field but 0 in the DSL — the DSL
      default would break the plane family). `mapping.apply_brush` copies
      the active kernel's values into the engine fields per dab, after the
      mapping table so table-driven fields keep authority. Gotchas: the
      reflected string member needs `read_litestl_string(entry.name.ptr)`;
      `entry.def` needs `getattr` (Python keyword); the reflected int
      primitive is `int32`. Current yield: kelvinlet `mu`/`nu`, plane-family
      `planeSide`. Verified (`tests/engine_props_test.py`, ALL PASS):
      generation, apply routing, `nu` behaviorally reshapes the elastic
      field (`mu` is normalized out by the kernel by design), CLAY behavior
      unchanged with generated defaults, disable/re-enable idempotent.
- [x] M3 **done** — the N-panel Brush section gained an auto "Engine"
      subsection listing the active kernel's generated props (GUI-verified:
      Elastic Deform shows mu/nu at engine defaults). Brush + Dyntopo +
      Multires panels already existed.
- [x] M4 **done** — Pressure: the stroke operator configures the engine's
      per-stroke device dynamics through the int-keyed ids (the string API
      can't marshal `util::string`; constants in `mapping.py`) —
      `use_pressure_strength`/`use_pressure_size` → identity-curve MULTIPLY
      layers on strength/radius — and refills the device samples with
      `event.pressure` per dab. Grab-class strokes get no dynamics (they
      push no samples; a configured layer would apply stale device state).
      Engine fix en route (submodule): `execBrush` never resolved common
      props through the dynamics stack (only `execProgram` did), so pushed
      samples were inert on plain dabs; it now runs `loadCommonProps(ctx)`
      before exec (bit-identical no-op without configured devices; kernel
      uniforms keep raw-field semantics — no `loadUniformProps` there, our
      mapping sets them as fields). Verified:
      `claudeMemory/tests/pressure_dynamics_test.py` (single-dab strength
      ratio exactly 0.30 at pressure 0.3; radius footprint 29→9 verts at
      0.5; samples inert without dynamics) + GUI operator path at mouse
      pressure 1.0 (configured no-op). Engine ctest: 6 local failures are
      pre-existing (A/B-verified against the reverted change — missing WGSL
      backend config + two stale tests). Real-pen feel test still owed
      (simulated events cannot carry pressure). Autosmooth program and
      pixel-radius unprojection landed earlier (P3 A3 / P4 S2).
- [x] M5 Per-brush-type parity harness (headless) — dabs each supported
      brush on a sphere and asserts the expected effect (DRAW/SHARP/INFLATE
      net-outward; CLAY/PLANE/SCRAPE/PINCH/SMOOTH move verts; MASK leaves
      positions put), plus a guard check that every `UNSUPPORTED` type is
      refused and `apply_brush` runs against real Blender brushes without a
      field-name error. Formalized as
      `claudeMemory/tests/brush_parity_test.py` (run via `run_sync.py`;
      NaN-proof guards per the ASAN-refresh convention). All 13 checks pass
      on RelWithDebInfo and ASAN-clean on the MSVC ASAN build.
- [~] Phase 2 — **brush textures DONE** (plan:
      [brush-textures.md](./brush-textures.md)): engine seam is bindings-only
      (`Brush.setTexture/clearTexture` + reflected
      `coord_space`/`tex_repeat`/`tex_width`/`tex_height`, a
      `TexCoordSpace` enum binder, `CommandExecutor.setRenderMatrix`); addon
      `texture.py` bakes `Texture.evaluate` intensity over `[-1,1]²` at
      128², caches by name (depsgraph + load_post invalidation), maps
      `map_mode` → `TexCoordSpace` (3D/VIEW_PLANE/TILED/AREA_PLANE;
      RANDOM/STENCIL unmapped → cleared), and the stroke operator binds per
      stroke (+ perspective matrix for the view-pinned modes). N-panel
      Texture section. Gate: `tests/brush_texture_test.py` (gradient
      modulates DRAW, symmetric control, cache invalidation, unmapped
      clears). Parity limits documented in the plan (Projected UV is
      world-units, DRAW-family kernels only, no mtex extras).
      **GUI-verified by the user** — texture brush drawing works in the
      viewport, closing the interactive mapping check. Three fixes came out
      of that pass: the paint context now resolves for custom modes
      (`OBJECT_MODE_TYPE_USE_SCULPT_PAINT`, so the properties-editor Texture
      tab appears), the view-pinned UV gained the missing perspective divide
      / NDC remap and Blender's brush-centered View Plane maps to the
      engine's normalized Projected space, and custom-mode entry now runs
      `BKE_paint_init` so a fresh session has an active brush.
      **Cavity automask DONE**: `mapping.cavity_settings` resolves
      `MeshAutomaskingSettings` with Blender's precedence (brush cavity flags
      win over the Paint-level ones) and `_apply_cavity` copies
      enable/inverted/factor/blur-steps and bakes the custom curve into the
      engine's 256-entry LUT; the stroke operator passes the owning Paint.
      N-panel Automasking section. Gate:
      `tests/cavity_automask_test.py` — measures the mask directly by
      dividing each configuration's dab by an unmasked baseline (falloff
      cancels, so the ratio *is* the factor) on a grooved plane: concave
      floor 0.979, convex shoulder 0.016, flat exactly 0.500, inverted
      mirrors both about 0.5, `cavity_factor = 0` is a uniform 0.5, and a
      constant-1 custom curve restores full strength. The measurement also
      corrected the engine's `cavityRemap` doc comment, which had the
      convention backwards — concave pushes toward 1 (the effect stays in
      cavities), matching Blender's identical estimator.
- [ ] Parity checklist maintained for unmapped features (cloth/boundary/
      multiplane, topology rake, front-face, accumulate, tip shape,
      mtex RANDOM/STENCIL map modes + brightness/contrast/invert, and the
      non-cavity automasking modes — topology, face sets, boundary, view
      normal, start normal). Symmetry, `stroke_method`
      (ANCHORED/DRAG_DOT), and the SMOOTH/BSMOOTH kernel choice moved to
      **P9**; AIRBRUSH/LINE/CURVE stroke methods and radial symmetry stay
      on this checklist (deferred by P9).

---

## P8 — Multires conversion

Full plan → **[multires-convert.md](./multires-convert.md)**. Ignore the
multires modifier at runtime; convert via **absolute top-level positions**
(never frame-to-frame): MDISPS + limit surface → level positions → SculptCore
writeback cascade; export via reshape-context bake back into `CD_MDISPS`.

- [~] P0 Grid-correspondence investigation → **written + numerically probed**,
      `../research/grid-correspondence.md`. Both layouts pinned from source: one
      grid per face corner (quad→4, n-gon→n), same face/corner enumeration,
      **no level offset** (both `2^(L-1)+1` samples/edge, same numbering), grid
      `(0,0)` = corner vertex and far diagonal = face center in *both*. Bijection
      = grid-index identity + per-sample **identity-or-transpose**, over an
      absolute-position CCG-shaped interchange buffer (`abs[grid][y*side+x]`).
      **Numeric result (§5a):** SculptCore's zero-disp base is **discrete CC
      refinement**, Blender's multires base is the **CC limit surface** — they
      differ O(4⁻ᴸ) but converge to the *same* limit (no rule divergence on the
      cube). Benign for the absolute-position round-trip by design (base gap
      absorbed into the displacement channel; identity round-trip preserved).
      Still open: intra-grid transpose + corner parity — Blender exposes no
      per-grid positions to Python, so pin them at **Workstream B** via the
      bake round-trip oracle (no extra scaffolding). Crease-cage case still owed.
- [x] A1 `Multires_fromLevelPositions` + A2 `Multires_levelPositionsOut` /
      `Multires_levelSampleCount` **done** (`subdiv_c_api.cc`). A2 dumps level
      grid samples grid-major row-major; A1 seeds a level from that layout and
      writes back. Bit-exact seed→writeback→re-dump round-trip on a
      sphere-displaced cube, L1–L4 (§5b, `scripts/p8_roundtrip.py`). Detail lands
      at the seeded level; down-refit redistribution deferred.
- [x] A3 Vertex correspondence — **done as nearest-neighbour by base position**
      (SculptCore `Mesh_toArrays` level verts ⇄ Blender subdiv verts; equal-count
      dedup sets → perfect bijection, gap = discrete↔limit offset ≪ spacing).
      Full **export round-trip validated** L2–4 (§5e, `scripts/p8_export.py`):
      SculptCore surface → NN map → B2 bake → tear-free MDISPS. Import is the
      mirror (inverse map → A1). Cache the map per session; assert bijectivity.
      (Grid-sample C entry not needed — the dedup vertex path replaced it.)
- [x] A4 Grid paint-mask channel in/out **done** — Blender seam:
      `multiresModifier_maskFromVertValues` / `_maskToVertValues` (mask twin
      of the dedup-vertcos reshape walk in `multires_reshape_vertcos.cc`;
      masks are absolute so it is a direct top-level assignment — writes
      ensure/resize the `CD_GRID_PAINT_MASK` grids, reads bilinear-sample
      each grid at its own stored level) + RNA
      `Object.multires_mask_from/to_vert_values` (the identifier `values` is
      Python-reserved in RNA — `mask_values`). Addon: `MultiresMap` gains
      `engine_vert_to_blender` derived from `levelGridVertsOut` (no extra
      KD pass; note the reflected primitive is `int32`, not `int`);
      `multires.import_mask`/`export_mask` route `.spatial.v.mask` ⇄ grid
      mask through it. Exchange at the top level only: enter seeds, flush
      exports when active==top, and a level switch persists/re-seeds around
      the slot eviction. Verified (`scripts/p8_mask.py`, ALL PASS):
      vanilla-authored mask imports exactly, MASK brush paints, exit/
      re-enter round-trips through the grids, fully-masked verts resist a
      DRAW stroke at zero movement, level round trip preserves the mask.
- [~] B Blender bake seam **implemented**: `multiresModifier_reshapeFromPositions`
      (`multires_reshape.cc`, per-grid-array assign mirroring the CCG path) +
      `Object.multires_reshape_from_positions` RNA (`rna_object_api.cc`). Works
      (bake takes effect). Used as the convention oracle (§5c,
      `scripts/p8_pin.py`): corner-anchor convention pinned — grid `g↔loop g`,
      intra-grid transpose `(x,y)=(v,u)` (geometry + 16-candidate brute force
      agree). **Open:** the naive per-grid feed tears at grid seams (transpose
      reverses boundary order). **Resolved (§5d):** added B2
      `multiresModifier_reshapeFromVertPositions` +
      `Object.multires_reshape_from_vert_positions` (dedup subdiv-vertex feed) —
      **exact reproduction, seam-clean** (`scripts/p8_vertcos.py`). This is the
      production bake seam; the per-grid variant stays as a lower-level utility.
      Remaining: the SculptCore-vertex ↔ Blender-subdiv-vertex map (A3) — NN by
      base position, or a subdiv-vertex→grid dump; decide at C1.
- [x] C1/C3 **done** — `sculptcore_addon/multires.py`: `modifier()` detect,
      `build_engine()` (cage → `Multires_new`), `build_map()` (engine grid samples
      ⇄ Blender subdiv verts by NN on a throwaway zero-disp base reference),
      `import_displacement()` (Blender top positions → A1) and `export_bake()`
      (engine top → dedup vertcos → B2). Full import→export round-trip on a
      *displaced* multires cube is tear-free at ~1e-7 both ways (L2–3,
      `scripts/p8_addon.py`) + engine decls in `engine.py`. **Session wiring
      done**: `convert.enter` branches to `_enter_multires` (top-level eval →
      stack import → modifier `show_viewport` suppressed, restored on exit),
      `flush` → `export_bake` into `CD_MDISPS`, `Session` owns the
      `Multires` + cage (active mesh/tree are non-owning views), map cached;
      mid-stroke throttle only refreshes the draw provider (`draw_refresh`) —
      the full bake runs on flush/exit/undo. `resync_if_diverged` compares the
      cage. Engine fix en route: `Multires_fromLevelPositions` now
      rematerializes the seeded slot (its tree/normals were built from
      pre-seed positions — raycast/filterNodes would miss; import goes
      bit-exact → ~2e-7 through the store, by design). Verified headless
      (`scripts/p8_session.py`: lifecycle, identity round-trip 2e-7, 30-dab
      sculpt lands in MDISPS, modifier restore, no leaks) **and GUI**
      (provider draws the imported surface, dabs update live, after exit
      vanilla multires shows the baked edit at view levels 1–3).
- [x] C2 Level UI **done** — the modifier's `sculpt_levels` drives the engine
      level: a `depsgraph_update_post` handler (handlers.py) compares it to
      `session.multires_active_level` and calls `convert.set_multires_level`
      (engine `Multires_setActiveLevel` writes the outgoing level back to the
      store); `_rebind_multires_views` re-fetches the slot mesh/tree, resets
      the cached wrappers + meshlog (generation bump orphans old undo steps,
      like a refresh) and re-registers the draw provider tree. Enter honors
      `sculpt_levels`; `_flush_multires` restores the sculpt level after the
      top-level export dump moves it. N-panel "Multires" exposes the slider.
      Verified headless (`scripts/p8_level.py`, ALL PASS: handler switch,
      rebind 386→98 verts, sculpt at L2, flush restore, cascade into the
      bake) and GUI (live provider redraw on switch L3→L1→L3, coarse edit
      rides the cascade). Subdivide/delete deferred.
- [x] C4 Undo **done** — three layers:
      (1) In-level stroke undo/redo exact via the P6 meshlog path + flush
      bake, no extra code (`scripts/p8_mundo.py`).
      (2) **Level-crossing undo** via store-snapshot fallback: `undo.push`
      captures `Multires_serializeStore` blobs per stroke (post-writeback,
      `blob_before`/`blob_after`, neighbours share one bytes object; size
      counts toward the undo limiter); when a step's meshlog died (level
      switch / earlier blob restore → generation mismatch), decode restores
      the blob at the step's recorded level (`convert.multires_restore_blob`
      → `restoreStore` + `setActiveLevel` + rebind). The **level switch
      itself** needs no custom step: the `sculpt_levels` property edit pushes
      a memfile step, and undo re-drives the engine through the depsgraph
      handler. `session.multires_last_blob` keeps the chain rooted at the
      landed state across undo/redo branching.
      (3) Enabler fix in `custom_mode_undo.cc`: **`ut->poll = nullptr`**
      (like SCULPT) — the old context poll made EVERY generic push (property
      edits, `ed.undo_push`) an inert CUSTOM step, so DNA edits made in-mode
      were unrecoverable. Generic pushes now fall to the memfile catch-all.
      Follow-up in `undo.decode`: flush on every final decode even when the
      cursor did not move — the correct-order rule may have decoded an older
      memfile below the step (replacing the Mesh) which the flush re-asserts.
      Verified: `scripts/p8_c4.py` (strokes at L3/L2 with a switch between;
      undo to the asset and redo back — every stop ~1e-7, level follows the
      history) + the FULL P6 suite re-run green (7 tests) + full P8 suite.
      Store-rewriting ops (down-refit, subdivide/delete) stay deferred with
      their features. Caveat: one full store blob per stroke (limiter-bounded;
      revisit if profiling demands per-level chunks).

### Verification (per plan §5)
- [x] Zero-displacement and no-stroke identity round trips — no-stroke
      identity through the full mode lifecycle at ~2e-7 (`p8_session.py`);
      **render comparison done** (`scripts/p8_render.py`): a displaced
      production-style asset through a full enter/exit session renders
      identically (max pixel diff = one 8-bit quantum, mean 0).
- [x] Edit round trip visible/correct in vanilla Blender at view levels
      1–3 (GUI, displaced multires cube; deeper levels with the corpus pass).
- [x] **Corpus done** (`scripts/p8_corpus.py`): quad cube, n-gon cube,
      triangle cone, open boundary grid, open cylinder, creased cube ×
      L2–4 — all bijective, import/export ~1e-7 except the creased cube's
      export (~5e-4, within tolerance; the NN reference is built uncreased
      while Blender's bake respects creases — carry creases into the
      reference/cage if tighter is ever needed). Deep levels + scale
      (`scripts/p8_scale.py`): L5–6 exact; a 1.1k-face sphere at L4 (289k
      subdiv verts) round-trips at 6e-7, enter cost ~12 s (KD-tree map build
      dominates at ~7 s — `_nearest` swapped from O(N·M) brute force to
      `mathutils.kdtree`, without which production counts were unreachable).

---

## P9 — Stroke quality & parity (reference-app insights)

Full plan → **[stroke-quality.md](./stroke-quality.md)**. The six high-value
gaps from
[../research/webgl-app-reports-insights.md](../research/webgl-app-reports-insights.md)
(the reference TypeScript app's integration + stroke-driver reports).
Addon-only — no engine or Blender C changes; all engine seams
(`setNeighborMode`, preview-dab API, BSMOOTH) verified reflected.

- [ ] Q1a `setNeighborMode(1)` (CSR ring-1 cache) on executor construction —
      A/B timing + identical-result check first, then adopt in
      `_ensure_executor`.
- [ ] Q1b SMOOTH vs BSMOOTH A/B on an open-boundary mesh; switch the
      SMOOTH mapping entry + Shift-smooth kernel + autosmooth chain if
      BSMOOTH is the boundary-preserving parity match (the reference app
      uses BSMOOTH for both); record the decision.
- [ ] Q2 Dyntopo cadence: remesh at its own arc-length spacing
      (`stroke_s - last_dyntopo_s >= dyntopo_spacing`), not every dab;
      plain program path when not due; spacing knob in the Dyntopo panel.
      Once Q4 lands, the due decision is made on the primary dab and
      cached for mirror images.
- [ ] Q3 Spline stroke smoothing: dependency-free `stroke_math.py`
      (centripetal Catmull-Rom → Bezier, 32-chord arc-length walk,
      sub-curve slice) + `StrokeSpacer` upgraded to the spline walk with
      cross-segment carry, 1-segment lookahead, first-dab raw emission,
      right-clamped trailing flush.
- [ ] Q4 Plane-mirror symmetry: `symmetry.py` (`SymAxisMap` 8-combo
      sign-flip table), applied in the operator above the spacer; mirrored
      centers re-raycast (grab-class mirrors anchor+cursor directly);
      per-mirror grab state; shared dyntopo-due; driven by the shared mesh
      symmetry flags; N-panel Symmetry section. Radial symmetry deferred
      (P7 checklist).
- [ ] Q5 ANCHORED / DRAG_DOT stroke methods via the engine preview-dab
      API (`beginPreviewDab`/`rollbackPreviewDab`/`commitPreviewDab`;
      mirrors share one bracket via `extendPreviewDab`): one live dab at
      any moment, commit before `endStep`, cancel rolls back and skips the
      undo push. ANCHORED = surface-hit anchor + anchor-time view-plane
      projection + drag-length radius; DRAG_DOT = one dab at the live
      cursor. AIRBRUSH/LINE/CURVE deferred (P7 checklist).

### Verification (per plan)
- [ ] Q1a timing A/B, identical results; Q1b boundary-behavior A/B
      recorded.
- [ ] Q2 remesh-call count ~ `strokeLen / dyntopo_spacing`, not dab count.
- [ ] Q3 `stroke_math.py` unit tests (arc-length accuracy, carry
      continuity); jittery-input A/B; event-rate invariance gate extended.
- [ ] Q4 symmetric-mesh strokes stay symmetric across axis combos; grab +
      dyntopo under symmetry; undo exact.
- [ ] Q5 `claudeMemory/tests/stroke_methods_test.py` — direct-vs-wander
      no-compounding (|wander − direct| < 10 % of direct delta), undo
      restores, anchored refuses empty-space start, cancel is a no-op;
      full P6 suite re-run green.
- [ ] Final GUI feel pass (symmetry + anchored) via
      `--enable-event-simulate`.
