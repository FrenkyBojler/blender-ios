# CLAUDE.md

Guidance for working in the Blender codebase. For the full documentation
set (repository map, paint mode system, brush assets, Python/bpy
registration, DNA/RNA, depsgraph, operators, GPU/draw, nodes, build/test,
and the custom-mode integration design), see
**[claudeMemory/README.md](./claudeMemory/README.md)**.

This file condenses the official Blender developer guidelines
(<https://developer.blender.org/docs/handbook/guidelines/>): C/C++ style and
best practice, Python style, GLSL style, license headers, commit messages,
release notes, `.blend` compatibility, testing, committer etiquette, and
reverting. Follow these when writing or modifying code here.

---

## This Project — SculptCore Integration (temporary; not part of the final PR)

This branch (`sculptcore`) integrates the new `extern/sculptcore` sculpting
engine into Blender.

**Goal.** Ship a full sculpt mode as an *addon* — a first-class mode with
proper undo support, render-system integration, a real enter/exit lifecycle
(surviving object/workspace switches), and efficient conversion to/from `Mesh`.

**Strategy.** Make the **minimal** modifications to Blender's addon / Python
registration system needed to allow a full sculpt-mode addon, rather than
hardwiring another mode into C. The investigation that maps exactly which
points must change (and the proposed `bpy.types.ObjectModeType` +
`OB_MODE_CUSTOM` + wrapped-undo design) lives in
[claudeMemory/design/addon-custom-modes.md](./claudeMemory/design/addon-custom-modes.md).

**Reference app.** `webgl-app-framework-reports/` (untracked) documents how a
sister TypeScript/WASM app integrates the same engine — a map of
battle-tested engine seams. The insights that transfer to this project are
distilled in
[claudeMemory/research/webgl-app-reports-insights.md](./claudeMemory/research/webgl-app-reports-insights.md)
and planned as P9
([claudeMemory/plans/stroke-quality.md](./claudeMemory/plans/stroke-quality.md));
prefer the distilled note, and treat the reports' `file:line` references as
belonging to the *other* codebase.

### Working conventions for Claude (this project only)

- Put everything Claude generates under `claudeMemory/`:
  - Implementation plans → `claudeMemory/plans/`.
  - Research notes → `claudeMemory/research/`.
  - Validated Blender codebase reference docs → `claudeMemory/codebase/`.
  - Design docs → `claudeMemory/design/`.
  - The index is [claudeMemory/README.md](./claudeMemory/README.md).
- When implementing a plan, prefix any scaffolding/helper comment with
  `CLAUDENOTE:` so it is trivially greppable, and **strip all `CLAUDENOTE:`
  comments before the plan is considered done.**
- After finishing a plan, **audit every comment you touched** (not only
  `CLAUDENOTE:` ones) against the code and the comment guidelines below;
  leave only accurate, guideline-conforming comments.
- **Pushing**: push this Blender repo to the `joeedh` remote
  (`git push joeedh sculptcore` — the JosephEagar fork), never to `origin`
  (upstream `blender/blender`).

### Building (this environment)

The build uses the presets in `CMakePresets.json` (Ninja + `clang-cl`,
`sccache` enabled). Default to the **`relwithdebinfo`** preset (optimized with
debug info). `clang-cl` is not on `PATH` and the presets need the MSVC
developer environment active, so run cmake through the wrapper
`claudeMemory/scripts/bl_env.bat`, which activates `vcvars64` and prepends the
clang-cl / cmake / ninja / sccache directories:

```
claudeMemory\scripts\bl_env.bat cmake --preset relwithdebinfo          # configure
claudeMemory\scripts\bl_env.bat cmake --build --preset relwithdebinfo  # build
```

Precompiled libraries live at `lib/windows_x64`. Build output lands in
`../build_windows_x64_clang_RelWithDebInfo` (the presets place build dirs
beside the source tree, not inside it); the runnable `blender.exe` is in that
tree's `bin/`. Other presets: `release`, `debug`, `asan`.

### Building the SculptCore engine for the addon (this environment)

The addon does **not** compile into `blender.exe`. It loads the engine at
runtime through a ctypes package (`extern/sculptcore/python/sculptcore/`) that
wraps a native shared library, `sculptcore_capi.dll`. So a change to the C++
engine (`extern/sculptcore/source/**`) reaches Blender by **rebuilding that
DLL** — Blender itself needs no rebuild. A change to the addon's Python
(`scripts/addons_core/sculptcore_addon/**`) needs nothing rebuilt at all.

Build + stage everything with the Node dispatcher (see
`extern/sculptcore/CLAUDE.md` for the full tool):

```
cd extern/sculptcore
node make.mjs bundle   # build the DLL, then vendor package + DLLs into the addon
```

`bundle` runs `build python` (skip with `--no-build`), then mirrors the
`sculptcore` ctypes package plus `sculptcore_capi.dll` / `wgpu_native.dll`
(`--pdb` adds the .pdb) into `sculptcore_addon/lib/sculptcore/` — in the
source tree **and** every sibling build tree's addon copy (the one
`blender.exe` actually runs). Deps (OpenBLAS/CHOLMOD) are statically linked
into the DLL. If a running Blender holds the old DLL, bundle renames it aside
and stages the new one; restart Blender to pick it up. The vendored `lib/` is
gitignored — it is a build product, never committed.

**Launch** — no env vars needed after a `bundle`:

```
Start-Process "C:\dev\blender\build_windows_x64_clang_RelWithDebInfo\bin\blender.exe"
```

**How the pieces are discovered**:
- The addon's `engine.py` imports the `sculptcore` package via, in order: an
  already-importable `sculptcore`; the directory in `$SCULPTCORE_PYTHON_PATH`
  (dev override beats the bundle); or the vendored copy at
  `scripts/addons_core/sculptcore_addon/lib/sculptcore/`.
- The package's `_capi.py` finds the DLL via, in order: `$SCULPTCORE_CAPI_PATH`;
  a copy beside the package (the bundled case — `wgpu_native.dll` resolves via
  `add_dll_directory` on that same directory); or
  `<sculptcore-repo>/build/python/` (where `build python` puts it — so a
  source checkout resolves automatically).

To iterate on the engine without touching the vendored copy, the old env-var
flow still works and takes precedence:

```
$env:SCULPTCORE_PYTHON_PATH = "C:\dev\blender\main\extern\sculptcore\python"
$env:PATH = "C:\dev\blender\main\extern\sculptcore\build\python;$env:PATH"
Start-Process "C:\dev\blender\build_windows_x64_clang_RelWithDebInfo\bin\blender.exe"
```

`engine.py`'s `init()` refuses an ABI-mismatched DLL; on any engine load
failure the addon reports it to the system console (Window → Toggle System
Console). To sanity-check the DLL without launching Blender:
`python -c "import sculptcore; sculptcore.init()"` (with the two env vars set).

The engine's own ctest suite (`node make.mjs build native` then
`node make.mjs test [name]`, run in `build/native`) is the authoritative check
for the CPU executor path the addon exercises; run it before rebuilding the DLL.

### Debugging (this environment)

- **Native C++** (SculptCore engine, C bridge): attach a debugger to
  `blender.exe`, or launch it. RelWithDebInfo ships full symbols in
  `../build_windows_x64_clang_RelWithDebInfo/source/creator/RelWithDebInfo/blender_private.pdb`.
  Ready-made VS Code configs (`cppvsdbg`) are in `.vscode/launch.json`.
- **Remote Python**: `claudeMemory/scripts/remote_repl.py` is a main-thread-safe
  TCP REPL (socket recv on a background thread, `exec` marshalled onto the main
  thread via `bpy.app.timers` -- `bpy` is not thread-safe). Launch with
  `blender.exe --python claudeMemory/scripts/remote_repl.py` (default
  `127.0.0.1:4444`); drive it with `remote_repl.py --client` or any socket
  tool (NUL-terminated commands). Set `BLENDER_DEBUGPY_PORT` to also start a
  `debugpy` listener for the "Python: Attach to Blender" VS Code config.
- `.vscode/launch.json` is dev scaffolding; delete `.vscode/` before the PR.

### Before the final PR (cleanup checklist)

This scaffolding is *not* part of the upstream contribution. Before opening
the final PR:

- **Delete `claudeMemory/`.**
- **Delete `webgl-app-framework-reports/`** (reference material from the
  sister app; untracked, never committed).
- **Bring back the old documentation set (`claudeDocs/`) and `AGENTS.md`:**
  - `claudeDocs/` was committed to this branch's history before deletion
    (commit *"Preserve pre-project claudeDocs for later restoration"*).
    Restore it from there (e.g. `git checkout <commit>~1 -- claudeDocs`, or
    `git checkout <that commit> -- claudeDocs`).
  - `AGENTS.md` was byte-identical to this file's coding-guideline sections
    (only the top title/intro differed). Recreate it by copying the reverted,
    guidelines-only `CLAUDE.md` under the heading `# AGENTS.md`.
- Revert this project-specific section and the doc-link change above, so
  `CLAUDE.md` returns to the upstream guidelines-only form.

---

## Two Overriding Rules

1. **Conform to the surrounding code.** When making changes, match the style
   and conventions of the surrounding code, even if it differs from these
   guidelines. When an area doesn't follow the guidelines, prefer following
   the existing local style. Do not mix functional and style changes in one
   commit.
2. **Strive for clarity**, even if that occasionally means breaking a
   guideline. Use your head; ask for advice when common sense disagrees with
   the convention.

Formatting (C/C++/GLSL via **clang-format**, Python via **autopep8**) is
automated — run `make format` or configure your IDE. The rules below cover
what is *not* automated.

---

## C / C++ Style

### Language & Encoding

- **American English** spelling for all doc-strings, variable names, comments.
- Use **ASCII** where possible; avoid special Unicode (`÷`, `¶`, `λ`). Use
  UTF-8 only where Unicode characters are genuinely required.
- Unix line endings (`LF`, `'\n'`).
- Strip trailing whitespace (configure your editor to do it on save).
- Indent with **2 spaces** in C/C++.

### Naming

- `snake_case` for variables and functions. Use descriptive names for globals
  and functions; keep local variables short and to the point.
- **Public functions** include the module identifier in all caps, the object/
  property operated on, and the operation — like RNA callbacks:
  ```c
  /* Don't: */ ListBase *curve_editnurbs(Curve *cu);
  /* Do:    */ ListBase *BKE_curve_editnurbs_get(Curve *cu);
  ```
- **Private (static) functions** must not start with a capitalized module
  identifier; a lower-case module prefix is fine:
  ```c
  /* Don't: */ static void DRW_my_utility_function(void);
  /* Do:    */ static void drw_my_utility_function(void);
  ```
- **Macros & constants**: ALL CAPS.
- **Enum labels**: ALL CAPS. Enums used in DNA must have explicit values.
- **C++ namespaces**: lower case (see below).

#### Size / Length / Count suffixes

| Suffix | Meaning |
|--------|---------|
| `_num` | Number of items in an array/vector/container. |
| `_count` | Accumulated/counted values (e.g. items in a linked list). |
| `_size` | Size in **bytes**. |
| `_len` | String length (excluding null byte, as `strlen` returns). |

```c
int *lut;        /* allocated C array */
int lut_num;     /* number of elements in `lut` */
size_t lut_size; /* allocated size of `lut`, in bytes */
void function(int *lut, int lut_num);
```

Use the same suffixes for functions returning such values
(`BLI_listbase_count`, `BLI_dynstr_len`). **Exception**: generic C++ containers
(`std::`, `blender::`) use a `size()` method for item count.

#### Function return arguments

C functions returning values via arguments:
- Prefix with `r_` (e.g. `r_center`) to denote a return value.
- Group them at the **end** of the argument list.
- Optionally put them on a new line when the list is long.
```c
/* Don't: */ void BKE_curve_function(Curve *cu, int *totvert_orig, int totvert_new, float center[3]);
/* Do:    */ void BKE_curve_function(Curve *cu, int totvert_new, int *r_totvert_orig, float r_center[3]);
```
(Some areas use a `_r` *suffix* like `center_r`; this is **not** the
convention but is left unchanged for now.)

#### C++ class data members

- **Private/protected** data members get a **trailing underscore** (`my_float_`).
- **Public** data members do **not**.

### Value Literals

```c
float foo = 0.3f;   /* not .3   */
float bar = 1.0f;   /* not 1.f  */
bool  ok  = true;   /* not 1    */
bool  no  = false;  /* not 0    */
```
Use `f` suffix for floats only (not doubles).

### Integer Types

> Lots of existing code predates these rules. Don't do global replacements
> without talking to a maintainer. When interfacing external libraries,
> following their type policy may be preferable.

- Of the builtins, use only `int` and `char`. Instead of `short`/`long`/
  `long long`, use fixed-size types (`int16_t`, etc.). Assume `int` ≥ 32 bits.
- Use `int64_t` for integers that can get "big". If a container's size could
  be large, use a large enough size type (when in doubt, `int64_t`).
- Use `bool`/`true`/`false` for truth values, not `int` 0/1.
- Use **unsigned** integers for bit manipulation and modular arithmetic
  (note modular arithmetic in a comment). Always use sized unsigned types
  (`uint8_t`…`uint64_t`). Flags should be fixed-size unsigned.
- Don't use unsigned to merely indicate non-negative — assert instead.
- If code already uses `uint`, avoid arithmetic on it (small positive
  additions are OK; avoid subtraction or anything possibly negative).
- For pointer-as-integer, use `intptr_t` / `uintptr_t`.

### Statements & Operators

**Switch:**
- Each `case` block must end with `break` or `ATTR_FALLTHROUGH;` (so a missing
  break is never ambiguous).
- When a `case` uses braces, the `break` goes **inside** the braces.
- Use braces only when introducing `case`-local variables.
```c
switch (value) {
  case TEST_A: {
    int a = func();
    result = a + 10;
    break;
  }
  case TEST_B:
    func_b();
    ATTR_FALLTHROUGH;
  case TEST_C:
  case TEST_D:
    func_c();
    break;
}
```

**Braces — always use them**, even for single statements:
```c
if (a == b) {
  d = 1;
}
else {
  c = 2;
}
for (int i = 0; i < 3; i++) {
  dest[i] = src[i];
}
```

**Variable scope**: keep it as small as possible — declare at first use rather
than declaring up front and assigning later.

**`const`**: use it whenever possible. Prefer declaring new variables over
mutating existing ones so `const` *can* be used. (In a function *declaration*,
`const` on a by-value parameter is irrelevant and only meaningful in the
definition.)

### Comments

- Third person, to the point, same terminology as the code (good technical
  documentation style).
- Explain non-obvious algorithms, hidden assumptions, implicit dependencies,
  and design decisions with their reasons.
- Acronyms in upper case (`API`, not `api`).
- Proper sentences: capitalized, ending with a full stop.
  ```c
  /* My small comment. */   /* not: my small comment */
  ```
- C-style comments (`/* */`) even in C++. For deliberately unused code, use
  `//` for single lines and `#if 0` for multi-line, and always explain it.
- Multi-line comments mark every line with `*`:
  ```c
  /* Special case: ima always local immediately. Clone image should only
   * have one user anyway. */
  ```
- Back-tick code/non-English text: ``/* The expression `x->y / 2`. */``
- Reference symbols (functions, structs, enum values) with a leading `#`
  (doxygen autolink): `/** Remove by #wmGroupType.type_update_flag. */`
- Email addresses in angle brackets: `Full Name <name@addr.com>`.

**Tags**: `/* TODO: body text. */`, optionally `TODO(@username)` or
`TODO(#123)`. Common tags: `NOTE`, `TODO`, `FIXME`, `WORKAROUND` (use instead
of `HACK`), `XXX` (general alert — prefer a more descriptive tag). Tags should
describe the problem *and* how it might be fixed, not just flag it.

**Section comments** use doxygen `\name` groups:
```c
/* -------------------------------------------------------------------- */
/** \name Title of Code Section
 * \{ */

... code ...

/** \} */
```

**API docs** use doxygen syntax:
```c
/**
 * Return the unicode length of a string.
 *
 * \param start: the string to measure the length.
 * \param maxlen: the string length (in bytes)
 * \return the unicode length (not in bytes!)
 */
size_t BLI_strnlen_utf8(const char *start, const size_t maxlen);
```

**Where documentation lives:**
- **Public symbols declared in a header** are part of the module's public
  interface — document them **in the header**. Aim to let developers use a
  module by reading only its header (black-boxing). Don't expose internal
  implementation details there.
- **File-internal** symbols (`static`, anonymous namespace) are documented at
  the implementation.
- **Implementation details** are documented at the definition (or even inside
  the function).
- Avoid duplicating docs between header and implementation; refer from the
  internal symbol to the public one. Only put `\param`/`\return` in the public
  doc-string (doxygen can't handle them defined twice).

### Clang-Format

clang-format is required for C/C++/GLSL. Disable only locally where it
produces significantly worse output:
```c
/* clang-format off */
... manually formatted code ...
/* clang-format on */
```

### Utility Macros (`BLI_utildefines.h`)

Avoid wrapping logic in macros, but these standard ones are encouraged:
- `SWAP(type, a, b)` — swap two values (prefer `std::swap` in C++).
- `ELEM(value, a, b, …)` — does value match one of the listed values.
- `POINTER_AS_INT` / `POINTER_FROM_INT` — warning-free int/pointer conversion.
- `STRINGIFY(id)` — identifier to string.
- `STREQ`/`STRCASEEQ`, `STREQLEN`/`STRCASEEQLEN` — clear string comparison.
- `AT` — `__FILE__:__LINE__` convenience.
- `BLI_assert(test)`, `BLI_assert_unreachable()`.
- `BLI_INLINE` — portable inline prefix.

### File Conventions

- Keep files roughly **under 4000 lines** (rule of thumb, not hard limit);
  consider splitting larger files logically.
- **Extensions**: C → `.c`/`.h`; C++ → `.cc`/`.hh` (`.cpp`/`.hpp`/`.h` exist
  but prefer `.cc`/`.hh` in new code). Keep a module consistent.

---

## C++ Specifics

### Namespaces

- Lower case names. Top-level `blender`; most code in nested namespaces like
  `blender::deg` or `blender::io::alembic`. Common `blenlib` data structures
  may live directly in `blender` (e.g. `blender::float3`).
- Prefer `namespace blender::io::alembic { … }` over nested blocks.
- Tests live in the same namespace as the code under test.
- **Anonymous namespace**: prefer `static` for file-private functions (the
  scope is visible locally). Use the anonymous namespace for file-private
  variables and class declarations.
- **Unity builds**: put compile-unit-private symbols in
  `blender::<module>::unity_build_<file>_cc` to avoid symbol conflicts when
  files are concatenated.

### Containers & Strings

- Prefer Blender's own containers (`blender::Vector`, `Array`, `Set`, `Map`)
  over the `std::` equivalents.
- Pass `blender::Span<T>` / `MutableSpan<T>` **by value** as parameters rather
  than `const Vector<T> &` / `const Array<T> &`.
- Format strings with the **fmt** library (`#include <fmt/format.h>`), not
  `std::format`.

### Language Features

- **No C++20 modules** — use normal header files.
- **No C++20 coroutines** — no justifying use-cases currently.

### Type Casts

- For arithmetic and enum types, use functional-style casts:
  `int(float_value)`, `float(int_value)`.
- For other conversions use `static_cast` when possible, else
  `reinterpret_cast` / `const_cast`.
- **Down-casting polymorphic types** — decision order:
  1. Can you avoid the downcast (e.g. virtual methods)? → don't cast.
  2. Need a type check? → `dynamic_cast` to a **pointer**; always check the
     result.
  3. No check needed, performance-sensitive? → `static_cast` (fast, but a
     wrong assumption is a hard-to-find bug).
  4. No check needed, not performance-sensitive? → `dynamic_cast` to a
     **reference** (throws if the type is wrong).

### `auto` and Type Deduction

- Don't use IDE hover help as an excuse to drop explicitness; code must be
  readable in plain review (e.g. online PR review).
- `auto` only when the type is clear from the same expression (e.g. a cast),
  for unnamed types (lambdas), or for iterators used through normal patterns.
  ```c++
  auto *var = static_cast<blender::Map<std::string, ID *> *>(user_data); /* OK */
  auto result = my_callback(my_id);  /* Don't — type unclear */
  for (int v : my_array) { … }       /* prefer over `auto v` */
  ```
  Exception: `enumerate()` requires `auto` for the `[index, item]` pair.
- **Template type deduction**: be explicit when types are concise and clarity/
  safety benefit; rely on deduction when types are verbose, non-local, or
  obvious. Templated calls whose template args affect the return type should
  usually specify them explicitly. Specifying a `Value` parameter (e.g.
  `threading::parallel_reduce<int>(...)`) while letting callback types deduce
  is a good balance.

### Class Layout

```c++
class X {
  /* using declarations */
  /* static data members */
  /* non-static data members */

 public:
  /* constructors (default, other, copy, move) */
  /* destructor */
  /* assignment operators (copy, move), other operators */
  /* public static methods, then public non-static methods */

 protected:
  /* protected static, then non-static methods */

 private:
  /* private static, then non-static methods */
};
```

### `this->`

Use `this->` when accessing members/methods **without** a trailing underscore;
do **not** use it for trailing-underscore (private) members:
```c++
void foo() {
  this->my_int = 42;   /* public, no underscore → use this-> */
  this->bar();
  my_float_ = 3.14f;   /* private, trailing underscore → no this-> */
}
```

---

## C / C++ Best Practice

- **`sizeof` first**: write `sizeof(type) * length` to promote the second
  operand to `size_t` and avoid integer overflow. For array allocation use
  `MEM_malloc_arrayN` / `MEM_calloc_arrayN`.
- **Unused C++ args**: comment the name out — `int /*my_unused_var*/` — rather
  than the `UNUSED()` macro (which doesn't suppress the warning on MSVC).
- **Avoid unsafe string C-APIs** (historic source of bugs). Use safe
  alternatives:

  | Unsafe | Safe |
  |--------|------|
  | `strcpy`, `strncpy` | `BLI_strncpy`, `STRNCPY` |
  | `sprintf`, `snprintf` | `BLI_snprintf`, `BLI_snprintf_rlen`, `SNPRINTF`, `SNPRINTF_RLEN` |
  | `vsnprintf`, `vsprintf` | `BLI_vsnprintf`, `VSNPRINTF`, `VSNPRINTF_RLEN` |
  | `strcat`, `strncat` | `BLI_strncat`, `BLI_string_join` |

  Fixed-size char buffers must be null-terminated (queries like `strlen`,
  `strstr` relying on that are fine). For UTF-8 that may not fit, use
  `BLI_strncpy_utf8` or strip with `BLI_str_utf8_invalid_strip`. Prefer
  `memcpy` for low-level byte work; assert the final size fits.
  *Exceptions*: `extern/` libraries, and `StringPropertyRNA::get` callbacks.

---

## Python Style

Follow **PEP 8** with these clarifications:
- 4-space indentation, no tabs. Unix line endings.
- Spaces around operators (except keyword arguments).
- `CamelCase` for classes and exception types; `underscore_case` for
  everything else.
- Most code is auto-formatted with **autopep8** (`make format`).

**Naming additions:**
- Don't shadow Python built-ins (functions, constants, types, exceptions). Use
  `obj` not `object`; prefer specific names (`objects_to_export`, not `list`).
- Avoid overly short names: `mesh`/`curve`, not `me`/`cu` or `m`/`c`.

**Unused variables/arguments**: prefix with `_` (e.g. `_context`, `_edge`) so
linters skip them.

**Exceptions to PEP 8:**
- **Line width: 120** for all scripts.
- Imports are often placed inside function bodies (not PEP 8 compliant) to
  speed up Blender startup.

**Core scripts** (`scripts/startup`, startup parts of `scripts/modules`) have
extra rules:
- Postpone module imports (put them in function/method bodies) for fast
  startup and lower overhead in background/test/render.
- **No type annotations** except those required by `bpy.props`.
- Use `str.format(...)` (positional args, type specifiers `{:s}`, `{:d}`,
  `{:f}`, `{!r}`) — **not** f-strings or `%` formatting (string literals often
  need translation; one consistent method keeps the code-base simple).
- **Single quotes for enum literals** (`ob.type == 'MESH'`), double quotes for
  everything else (`layout.label(text="Label Text")`).

---

## GLSL Style

Start from the C/C++ style guide; the points below are the differences. Run
auto-format on `.glsl` files. (Much existing code predates these rules — don't
mass-replace without consulting a maintainer.)

**Files:**
- Suffix by stage: `_vert`, `_frag`, `_geom`, `_comp`
  (e.g. `eevee_film_frag.glsl`). Library files (no `main()`) use `_lib`.
- File names must be unique and prefixed by their module
  (`workbench_material_lib.glsl`). Exactly one `main()` per non-library file.

**Naming:** `snake_case` (except type names like `ViewMatrices`). GLSL has one
global namespace — prefix every `_lib.glsl` function with the library name.
Common words first, specifics as suffixes, sorted alphabetically. Don't use
reserved keywords (`sampler`) as names.
- Texture coords: `uv`/`coord` for normalized, `texel` for integer pixel
  coords.
- Shading vars (unit vectors except `P`): `P` position, `N` shading normal,
  `Ng` geometric normal, `T` tangent, `B` bitangent/binormal, `L` light dir,
  `V`/`I` view vector.
- Space prefixes (single char on shading vars): none = world, `v` view,
  `t` tangent, `l` local. General two-char prefixes: `ws_`, `vs_`, `ls_`,
  `hs_` (homogeneous/clip), `ndc_`.

**Literals:** floats always `0.3f`/`1.0f`; uints always `0xFFu`/`0u`.

**Vectors/Matrices:** uniform argument types in multi-scalar constructors
(`vec2(2.0, 0.0)`); matrices use all-scalar or all-column form. Use swizzles
(`.x`, `.y`…) not `[]` (except runtime random access); prefer `.xyzw`
(`.rgba` when meaningful); never `.stpq` (no Metal support).

**Comparison:** no direct vector `==` — use `is_equal(a,b)`/`is_zero(a)`. No
cross-type comparison — cast explicitly.

**Types:** use HLSL/MSL/Blender vector & matrix types (`float2`, not `vec2`).

**Interface:** `snake_case` resource names; samplers `_tx`, images `_img`,
storage/uniform buffers `_buf`, fragment outputs / written resources `out_`,
read-write `inout_`/`in_` where meaningful.

**Defines:** prefer `const bool` + `if` over `#define`/`#ifdef` for branch
elimination; reserve `#ifdef` for code needing possibly-absent resources.

**Driver portability:** keep large arrays in local (function) constants, not
global. No implicit `int`→`float`/`uint`→`int` casts — cast explicitly. Avoid
multi-line `#if`. Don't shadow builtins (`distance`, `length`). `discard` must
be followed by `return` (Metal). Image-type opaque vars can't be function
parameters — use a macro or global. `bool` isn't allowed as a `shared` type
(use `int`/`uint`); vector components can't be atomic targets.

**Shared CPU/GPU files** (`.h`/`.hh`): use Blender's `floatX`/`intX`/`uintX`/
`boolX` and `float4x4`. Follow `std140`/`std430` packing: no `float3x3`, no
scalar arrays in UBO structs, use `packed_float3` (followed by a scalar),
`bool1` not `bool`; align vec2-types to 8 bytes, vec3/vec4-types and whole
structs to 16 bytes.

**Validation:** test Vulkan cross-compilation before opening a PR
(`WITH_VULKAN_BACKEND=On`, `WITH_GPU_BUILDTIME_SHADER_BUILDER=On`). Don't
assume Metal-only syntax works elsewhere.

---

## UI Messages

- **Channel identifiers** (X, Y, Z, R, G, B …) are always capitalized.
- No abbreviations — "vertices" not "verts", "vertex groups" not "VGroups".
- No contractions — "are not"/"cannot", not "aren't"/"can't".
- Data-block names may be Title Cased even in tips (a fuzzy rule — skip the
  emphasis if unsure).
- **UI labels**: English Title Case (each word capitalized).
- **Tooltips**: full sentences, but use the infinitive ("Make the character
  run", not "Makes…"), and **no** trailing period (so keep it one sentence —
  use commas/parentheses instead of multiple sentences).

---

## License & Copyright Headers

Every source file needs an SPDX copyright and license header.

**New files:**
```
SPDX-FileCopyrightText: 2024 Blender Authors

SPDX-License-Identifier: GPL-2.0-or-later
```
- Use the same license as other files in the folder (most are
  `GPL-2.0-or-later`; if different, tell the reviewer).
- `<Current Year> Blender Authors` for copyright.

**Editing files:** if `Blender Authors` copyright is missing, add a line with
the current year. Never remove existing copyright text. Extending the year
range on significant changes is optional.

**Adapting code from other projects:**
- Find the license identifier in `doc/license/SPDX-license-identifiers.txt`
  (if absent, discuss with the reviewer).
- Preserve the original `SPDX-FileCopyrightText`; add a `Blender Authors` line
  if you modified it.
```
SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al. All Rights Reserved.
SPDX-FileCopyrightText: 2011-2022 Blender Authors

SPDX-License-Identifier: BSD-3-Clause

Adapted code from Open Shading Language.
```

---

## Commit Messages

- First line = subject; blank line; then body. Lines ≤ **72 chars**.
- No embedded images; ASCII diagrams/arrows are fine.
- American English, technical-documentation style. Abbreviate only when
  well understood. Use people's full names, not nicknames.

**Three commit types:**

- **Bug fixes** — start with `Fix #12345: ` (or `Fix: ` for unreported bugs).
  Explain the fix at the user level (not just the code), and don't copy a
  non-descriptive bug title verbatim. `Fix #12345` auto-closes the issue.
  A workaround is categorized by area: `Cycles: Workaround for #123`, not
  `Workaround #123: …`.
  ```
  Fix #12345: Single short line explaining the bug at a user level

  Optional user-level detail; technical cause/solution if non-obvious.
  ```
- **Features / improvements** — start with a category (`Cycles:`, `Sculpt:`,
  `UI:`). Explain at the user level using UI names, separate from code notes.
  For performance, give specific numbers, not "slightly faster". `Ref #123`
  references, `Fix #123` closes.
- **Cleanup / refactor** — only when there are no functional changes. Keep
  cleanups in separate commits from functional changes. `Cleanup:` for
  removing unused code / fixing warnings / typos; `Refactor:` for
  restructuring/moving code.

**Authorship**: committing someone else's patch → use
`git commit --author "Full Name <Nick>"`, or add `Co-authored-by:` line(s)
(one per co-author).

---

## Release Notes

- Link the main commit/PR/issue at the end of each feature entry, after the
  full stop, in parentheses: `(blender/blender@<hash>)`,
  `(blender/blender#<issue>)`, `(blender/blender!<pr>)`. Link the matching
  manual version when possible.
- No abbreviations ("Grease Pencil", not "gpencil").
- Media: default theme, short videos, no Blender logo, no on-image text/arrows
  (explain in captions), minimal clutter, credit scene artists.
- Demo `.blend` files: include a `README` text data-block (instructions, CC
  license, author, URL); pack external data; test with F12; compress on save;
  no copyrighted/self-promo assets.

---

## `.blend` File Compatibility

A first-class concern — review before changing DNA.

- **Backward compatibility** (open older files) is near-guaranteed. A crash or
  corruption loading an older file is a severe/critical bug. Data loss is rare,
  pre-discussed, and well documented. Achieved by keeping deprecated DNA
  (tagged deprecated) and adding conversion in `do_version` paths with a
  version bump. Files from major version `n` should open without significant
  loss in `n` and `n+1`.
- **Forward compatibility** (open newer files) breakages are unavoidable but
  must never cause critical corruption/crashes, and the user must be warned of
  data loss. Non-critical = unknown new data (ignored). **Critical** = changed
  meaning of existing data (replacement, or in-place refactor of an existing
  DNA/ID) → major loss/crash in older versions.
- **Critical forward-compat breakages** are allowed only at the start of a new
  major cycle (~every two years, e.g. 3.x → 4.0). The previous cycle's latest
  LTS should still open and convert the new files.

**For development projects:** put compatibility handling in the design;
critical breakages need Core-team review and majority acceptance; tag the task
`Interest/Compatibility`; commit critical breakages only in the first months
of a major cycle; test repeatedly (open new files in the two maintained LTS
releases).

---

## Testing Changes & Refactors

- **Add/improve unit tests** in the area you touch when they're lacking.
- **Building**: a local build succeeding doesn't mean others' will. Ask the
  buildbot to build the PR by commenting `@blender-bot build` (needs commit
  access; reviewers do it otherwise).
- **Header-include changes** (refactors moving code, removing headers) can't
  rely on buildbot — Blender's default **unity builds** hide broken header
  dependencies. Locally **disable `WITH_UNITY_BUILD`** and do a **clean
  build**; a recent clang helps catch indirect-include breakage.
- **Automated tests**: run them locally during development
  (buildbot is the *last* stage). Also run debug + ASAN builds yourself
  (skip Cycles there — `ctest -E cycles` — they're very slow).
- **Manual testing** is mandatory for significant changes — the goal is to
  **break the new code**. Open complex production files and check for
  regressions; craft small demo cases; test compatibility by round-tripping
  files through older releases; get artists to stress-test features. Use both
  debug + ASAN (catches more) and release builds (perf and some concurrency
  bugs). `@blender-bot package` shares temporary builds with testers.

---

## Committer Etiquette (for those with write access)

- Commit only your own code or patches verified with the **module owner**.
  Check with the owner / a Development Coordinator before committing, and when
  unsure ask on chat.blender.org.
- Carefully inspect what you're about to push (`git status`, `git show`);
  pull recent changes; use interactive rebase to keep a clean, bisectable
  history of **not-yet-pushed** commits.
- Verify licensing/copyright (especially new files). Run clang-format before
  committing. Blender is strictly cross-platform — only commit code that
  compiles everywhere (use the buildbot when in doubt).
- Document features in the release notes around the time you commit them.
- Respect the **release cycle** — patches may be delayed near a release;
  don't make large changes just before one. After large commits, stay
  available 1–3 hours in case they break something. Reply to comments on your
  commits.

**Branches**: develop features in fork branches → contribute as pull requests.
Shared branches (multiple devs) may live in the main repo. Use clear,
lowercase, dash-separated names (`brush-assets-ui`). Avoid large binary files
(all devs fetch them). Delete branches once merged/obsolete; inactive ones are
archived.

---

## Reverting Commits

- Revert only **serious** breakages: corrupted `.blend` saves, broken
  regression tests, an unfixable broken build (or any reason the author finds
  reasonable).
- **No rush** — if a fix is coming soon, don't revert. Work with the team
  first; tests may just need updating (consult the area maintainer); allow
  time for platform-specific breakages.
- Be kind: talk to the author first; if they're offline and it's serious,
  revert. State the reason explicitly in the revert commit message and
  reference the relevant task numbers.

---

## Code Quality Day

First Friday of every month, the team focuses on code clarity rather than
bugs: adding comments, renaming obscure functions/variables, renaming DNA
members (with aliasing for compatibility), splitting large files, and
refactoring for clarity — tasks completable, reviewable, and committable
within a day.
