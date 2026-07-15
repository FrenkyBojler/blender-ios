# Plan — SculptCore Python Bindings

**Goal.** Give `extern/sculptcore` a first-class **Python** surface so the
Blender sculpt-mode addon can drive the engine from `bpy`/Python, mirroring the
existing TypeScript/WASM surface — **without** hand-writing a CPython extension
module. Python talks to the engine's already-existing `LSTL_*` `extern "C"` ABI
via `ctypes`; a generated `.pyi` stub set gives addon authors and the type
checker a static view of the dynamically-built classes.

This is the "Python support" first step of the project strategy (see
[../design/addon-custom-modes.md](../design/addon-custom-modes.md) for the
Blender-side mode/undo integration this feeds into). Task tracking lives in
[taskList.md](./taskList.md).

> **Revised after checking out `source/litestl`.** The binding system, the C++
> TS generator, and the `LSTL_*` C ABI are now confirmed in-tree; earlier
> assumptions (a to-be-written `binding_capi`, an N-API-based runtime port) were
> wrong and have been corrected below.

---

## 1. Architecture — where Python fits

SculptCore centres on one **runtime reflection registry**,
`litestl::binding::BindingManager`. C++ modules register every struct, member,
method, constructor, and enum into it; two engine entry points expose it:

- `extern "C" void initBindings()` — `source/core/bindings.cc` (calls each
  module's `registerBindings(manager)`).
- `extern "C" BindingManager *getBindingManager()` — `source/wasm/wasmManager.cc:7`.

Three consumers already sit on that one registry:

| Consumer | Location | How it reads the registry |
|---|---|---|
| **C++ code generator** | `litestl/binding/generators/typescript.cc` (884 lines), exported as `extern "C" LSTL_GenerateTypescript` (`binding.cc:384`) | walks `BindingBase*` descriptors directly; emits the `.ts` files (length-prefixed path→content buffer). |
| **TS/WASM runtime** | `litestl/binding/typescriptRuntime/` (`manager.ts`, `binding.ts`, `bind.ts`, `setValue.ts`, `boundVector.ts`, …) | drives dispatch through the `LSTL_*` C ABI; reads descriptor fields out of the WASM heap using the offset/size table from `LSTL_GetBindingInfo`. |
| **Native N-API runtime** | `source/napi/napi_runtime.{h,cc}` | reads descriptors directly in C++ (own marshalling). **Orthogonal to Python** — a separate native-JS path, not a dependency here. |

The **`LSTL_*` C ABI already exists and is complete** for dispatch
(`litestl/binding/CMakeLists.txt` `WASMSYM` list; bodies in `binding.cc`):

- Enumerate: `LSTL_Binding_GetKeys` / `LSTL_Binding_Get` / `LSTL_Binding_GetFullName`.
- Layout table: `LSTL_GetBindingInfo` → per-descriptor `offsetof`s + type sizes
  (e.g. `StructMember.{name,offset,type}`, `Struct.members`) — lets a caller
  read descriptor structs **without hardcoding C++ layout**.
- Methods: `LSTL_Struct_GetMethod{Count,}`, `LSTL_Method_Get{ParamCount,Param,Return,Name}`, `LSTL_Method_IsConst`, **`LSTL_Method_Invoke(m, self, args, ret)`**.
- Constructors / destructor: `LSTL_Struct_GetConstructor{Count,}`, `LSTL_Constructor_*`, **`LSTL_Constructor_Invoke`**, **`LSTL_Destructor_Invoke`**.
- Sizes + union disambiguation: `LSTL_GetBindTypeSize`, `LSTL_Union_HasDisPropFunc`, `LSTL_Union_RunDisPropFunc`.
- Alloc introspection: `LSTL_PrintAllocBlocks`, `LSTL_FormatBlock(s)`, `LSTL_GetMemSize`.

The thunk ABI behind `*_Invoke` is `void(*)(void *self, void **args, void *ret)`
(`binding_method.h:22`).

**Therefore Python is a fourth consumer that mirrors the TS/WASM runtime**: a
pure-`ctypes` port of `typescriptRuntime/` over the same `LSTL_*` ABI, plus a
new C++ generator sibling that emits `.pyi` the way `typescript.cc` emits `.ts`.

**The one real gap.** `LSTL_*` are exported **only to WASM**
(`lt_wasm_add_symbols(WASMSYM)`), and `binding` builds as a **STATIC** library.
`ctypes` needs a **native shared library** that exports those symbols plus
`initBindings`/`getBindingManager`. That native shared-lib target is the bulk of
the enabling C/build work — not a new dispatch layer (one already exists).

---

## 2. Design decisions

**Settled (recommended defaults; change only with reason):**

1. **No CPython extension module.** Python uses `ctypes` against the existing
   `LSTL_*` C ABI. Hot paths are already native (c-api factories, bulk
   pointer/vector views), so Python only orchestrates; bulk data crosses
   zero-copy as `numpy.frombuffer` views over engine pointers (numpy ships in
   Blender's Python).
2. **Reuse the `LSTL_*` ABI as-is** — do **not** build a new `binding_capi`; it
   already exists and the TS runtime proves it is sufficient. Read descriptor
   layout data-drivenly through `LSTL_GetBindingInfo` (mirror `binding.ts`), so
   Python never hardcodes C++ struct offsets.
3. **`.pyi` stubs are mandatory.** Classes are built dynamically off
   descriptors, so a type checker sees nothing without stubs — exactly the
   situation the generated `.ts` already addresses. Ship `py.typed` (PEP 561).
4. **Type checker: Pyright primary, mypy-clean secondary.** Best inference and
   `.pyi`/strict support, the Pylance engine (matches the existing VS Code
   toolchain), idiomatic in the Blender-addon ecosystem (`fake-bpy-module`).
5. **Emitter is a C++ sibling backend, not a TS→Py converter.** The authoritative
   generator is C++ (`generators/typescript.cc` → `LSTL_GenerateTypescript`). Add
   `generators/python.cc` + `LSTL_GeneratePython` beside it, reading the same
   descriptors. Rejected: parsing generated `.ts` (generics, mapped types,
   branded `Symbol`s, `UniformBindType`-style disambiguation) and lowering to
   Python — strictly harder and lossier than emitting from the structured source
   of truth. Union disambiguation lowers to `@overload`s straight from the union
   descriptor (`LSTL_Union_*`), just as the TS generator handles it.
6. **Naming: keep native (camelCase) names 1:1 across the boundary to start.**
   Simplest; the runtime name-transform can't drift from the emitter's. Optional
   snake_case aliasing is a later, additive layer.
7. **`ctypes`-only (D2).** A compiled `cffi`/pybind fast-dispatch shim stays a
   later, profiling-gated optimization — not built now.
8. **Self-hosted stub generation (D4).** `python -m sculptcore._gen` calls
   `LSTL_GeneratePython` on the shared lib it already loads; no Node `genPy` step
   paralleling `tools/genTS.ts`.

All decisions are locked at these recommended defaults for now — revisit only if
implementation surfaces a concrete reason. *(The earlier "D1 — unify N-API onto
a shared core" was dropped: N-API is an independent native path and the shared
dispatch ABI already exists as `LSTL_*`.)*

---

## 3. Change list by workstream

Sizes are rough (S ≈ <150 lines, M ≈ 150–500, L ≈ 500+). Work spans
`extern/sculptcore` and its now-checked-out `source/litestl` submodule.

### Workstream A — native shared library exposing `LSTL_*`

The enabling build change: produce a native `.so`/`.dll`/`.dylib` that exports
the existing ABI so `ctypes` can load it. No new dispatch code.

| File | Change | Size |
|---|---|---|
| `source/litestl/binding/CMakeLists.txt` | Add a **shared** flavour of `binding` (or an aggregate shared target) that force-exports `WASMSYM` on native builds. On Linux/macOS `extern "C"` + default visibility suffices; on **Windows** generate a `/EXPORT:` list (or `.def`) from the `WASMSYM` set. | M |
| `CMakeLists.txt` / `build_files/*.cmake` (sculptcore root) | New aggregate shared-lib target `sculptcore_capi` linking the engine + litestl `binding`, additionally exporting `initBindings` + `getBindingManager`. Reuse the native toolchain (clang; MSVC optional per local options). | M |
| `make.mjs` | `configure/build python` target → `build/python/`, producing `libsculptcore_capi.*`. Mirror the `node` target's flow. | M |
| `source/litestl/binding/binding.cc` | Add `LSTL_AbiVersion()` (+ bump on ABI change) so the Python package can refuse a mismatched lib. *(Optional convenience: `LSTL_Struct_GetMember*` accessors — not required; `LSTL_GetBindingInfo` already enables member reads.)* | S |

### Workstream B — Python `ctypes` runtime (port of `typescriptRuntime/`)

Direct port of the TS runtime, file-for-file where practical.

| New file (`python/sculptcore/`) | Ports / mirrors | Size |
|---|---|---|
| `_capi.py` | `wasmInterface.ts` + loader: `ctypes.CDLL`, `CFUNCTYPE` decls for every `LSTL_*`, `initBindings`/`getBindingManager`, ABI-version check, `LSTL_GetBindingInfo` offset/size table. | M |
| `_descriptors.py` | `binding.ts`: read `BindingBase`/`_StructBase`/`StructMember`/`Method`/`Constructor`/`Union` structs at `BindingInfo` offsets into Python objects. | M |
| `_classgen.py` | `bind.ts` + `manager.ts`: build one Python class per struct via metaclass; member get/set descriptors reading/writing at `this.ptr + member.offset`; method dispatch. | L |
| `_marshal.py` | `setValue.ts` + N-API `marshalArg` semantics: pack args into the `void**` thunk slots (scalars/enums/pointers into slots; refs/by-value structs point at the object), unpack returns via `getBoundPointer` logic; `LSTL_Method_Invoke`/`LSTL_Constructor_Invoke`. | M |
| `_bulk.py` | `boundVector.ts` / `vector.ts` / `string.ts`: zero-copy `numpy.frombuffer` views over engine pointers/Vectors; string + typed-array c-api seams (mirror the dedicated Mesh_* calls). | M |
| `_lifetime.py` | dispose/finalizer semantics: owning vs. non-owning wrappers, context-manager teardown, `LSTL_Destructor_Invoke` + free, double-free guard. | M |
| `_union.py` | union disambiguation via `LSTL_Union_HasDisPropFunc`/`RunDisPropFunc` (the `UniformBindType` case in `example.ts`). | S |
| `__init__.py` | import-time `initBindings()`; re-export generated classes; public surface. | S |

### Workstream C — `.pyi` emitter (C++ sibling of `typescript.cc`)

| File | Change | Size |
|---|---|---|
| `source/litestl/binding/generators/python.{cc,h}` (new) | Descriptor-driven `.pyi` emitter beside `typescript.cc`: structs→classes, members→typed attributes, methods→signatures (`@overload` for union-disambiguated params), enums→`enum.IntEnum`, dependency-ordered imports. | L |
| `source/litestl/binding/binding.cc` | `extern "C" LSTL_GeneratePython(BindingManager*, int *size_out)` + `LSTL_FreePythonString`, mirroring `LSTL_GenerateTypescript` (`binding.cc:384`). | S |
| `source/litestl/binding/generators/CMakeLists.txt` + `WASMSYM` | Build `python.cc`; add the new exports to the symbol list. | S |
| `python/sculptcore/_gen.py` (new; **D4** self-hosted) | `python -m sculptcore._gen` calls `LSTL_GeneratePython` on the loaded lib and writes the `.pyi` tree + `py.typed`; preserve any hand-written stubs (as `typescript/api` is preserved). | S |

### Workstream D — type-check + test harness

| File | Change | Size |
|---|---|---|
| `python/pyrightconfig.json` (new) | Strict Pyright over stubs + package. | S |
| `python/tests/test_smoke.py` (new) | `pytest`: import, `initBindings`, construct a `Mesh` primitive, read `bindingCount`, run a sculpt dab — Python analogue of `napi_smoke.cjs`, with **no C module built**. | M |
| `python/tests/test_parity.py` (new) | Match the TS/WASM runtime on a shared scenario (same registry ⇒ same result). | M |
| CI (`.github/`) | Pyright (gate) + mypy (secondary) + `pytest`. | S |

### Workstream E — Blender addon packaging (bridge to the mode design)

| Item | Change | Size |
|---|---|---|
| `python/sculptcore/` layout | Ship the shared lib + package + generated stubs as one importable unit loadable from Blender's Python; document load path + ABI-version guard. | M |
| link to [../design/addon-custom-modes.md](../design/addon-custom-modes.md) | Define the seam where the addon's mode enter/exit + wrapped-undo calls into this Python surface. | — |

---

## 4. Order of work

1. ~~Check out `source/litestl`.~~ **Done** — submodule populated; binding
   system, C++ generators, and `LSTL_*` ABI confirmed in-tree.
2. **Workstream A** — native `sculptcore_capi` shared lib exporting `LSTL_*` +
   `initBindings`/`getBindingManager` + `LSTL_AbiVersion`. Nothing loads without it.
3. **Workstream B (thin slice first)** — `_capi.py` + `_descriptors.py` +
   minimal `_classgen.py`/`_marshal.py`: load lib, enumerate, construct a `Mesh`,
   get/set a member, invoke one method. Proves the ABI end-to-end from `ctypes`.
4. **Workstream C** — `generators/python.cc` + `LSTL_GeneratePython` + `_gen.py`;
   regenerate `.pyi` alongside the `.ts`.
5. **Workstream B (complete)** — bulk/`numpy` views, Vectors, strings, unions,
   lifetime/dispose.
6. **Workstream D** — type-check + smoke/parity tests as CI gates.
7. **Workstream E** — packaging + the addon load seam.

---

## 5. Verification

- **ABI smoke:** `test_smoke.py` constructs and mutates a real engine object and
  runs a sculpt dab through pure `ctypes` — no C module built.
- **Parity:** `test_parity.py` matches the TS/WASM runtime on a shared scenario.
- **Type view:** Pyright strict passes on the generated stubs + an example addon
  script; stubs are also mypy-clean.
- **Bulk path:** a `numpy` view over a Vector round-trips vertex data zero-copy
  (verify via buffer-address identity — no copy).
- **Regeneration:** re-running `_gen.py` reproduces byte-identical `.pyi`
  (deterministic, like the TS output); `.ts` and `.pyi` describe the same ABI.
- **Lifetime:** owning wrappers free exactly once — `LSTL_GetMemSize`/alloc
  tracker reports zero leaks after teardown; double-free guard holds.

---

## 6. Things to confirm during implementation

- **Windows export list.** `extern "C"` exports auto-export from a shared object
  on Linux/macOS but need an explicit `/EXPORT:` list or `.def` on Windows —
  drive it from the `WASMSYM` set so it can't drift from the WASM export list.
- **Whole-engine link.** `sculptcore_capi` links the entire engine (like the
  WASM and node-addon targets already do); confirm the native deps
  (OpenBLAS/CHOLMOD) flow in via the existing `SCULPTCORE_DEPS_DIR` wiring.
- **`BindingInfo` completeness.** `_descriptors.py` relies on
  `LSTL_GetBindingInfo` covering every descriptor field it reads; extend the
  offset table if a needed field is missing rather than hardcoding a layout.

---

## 7. Decisions — locked at recommended defaults

**D2** (`ctypes`-only), **D3** (1:1 camelCase naming), and **D4** (self-hosted
stub gen) are settled at the §2 defaults for now. Reopen only if implementation
surfaces a concrete reason; note the reason here if so.

---

## 8. Risks

- **litestl is a shared sub-library** with a `constexpr` end-state goal; the
  shared-lib/export changes must not disturb its reflection model or the
  intended value-semantics direction (sculptcore CLAUDE.md → *litestl::binding*).
- **Cross-platform shared-lib loading** inside Blender's bundled Python —
  RPATH / DLL-search-path and ABI-version mismatch are the usual failures; the
  `LSTL_AbiVersion` guard mitigates.
- **`BindingInfo` offset drift** — mitigated because Python reads layout
  data-drivenly from `LSTL_GetBindingInfo` rather than hardcoding it (the same
  mechanism the shipping TS runtime relies on).
- **Pure-Python call overhead** on very chatty per-vertex loops — mitigated by
  keeping bulk work behind the c-api factories + `numpy` views, not by moving
  loops into Python (D2 fallback if profiling demands it).
