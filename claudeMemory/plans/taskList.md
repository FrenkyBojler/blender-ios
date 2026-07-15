# Master Task List — SculptCore Integration

Top-level checkbox tracker across all implementation plans for the SculptCore
integration (branch `sculptcore`). One section per plan; each links to the full
plan doc. Check items off as they land; keep phase order per the plan.

See [README.md](./README.md) for plan conventions and
[../README.md](../README.md) for the full memory index.

---

## Plans

| Plan | Status | Doc |
|---|---|---|
| Python bindings | Not started | [python-bindings.md](./python-bindings.md) |

---

## Python bindings

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
