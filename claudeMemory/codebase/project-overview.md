# Blender — Project Overview

Blender is the free and open source 3D creation suite, covering the entire
3D pipeline: modeling, rigging, animation, simulation, rendering, compositing,
motion tracking, video editing, and a 2D animation pipeline (Grease Pencil).

- **Language**: primarily C++ (with legacy C), Python for the UI/add-on layer,
  GLSL for shaders, CMake for the build system.
- **License**: GNU GPL v3 as a whole; individual files may carry compatible
  licenses (see `COPYING`, `doc/license/`).
- **Build**: CMake. Entry points: `CMakeLists.txt`, `GNUmakefile`,
  `make.bat`, `build_files/`.

This file is a map of the repository. For coding standards and contribution
rules see [CLAUDE.md](../../CLAUDE.md). For the full documentation index see
[README.md](../README.md).

---

## Top-Level Layout

| Path | Purpose |
|------|---------|
| `source/` | Blender application source (C/C++). The heart of the project. |
| `intern/` | Internal libraries developed alongside Blender but kept modular (memory, GHOST windowing, Cycles, etc.). |
| `extern/` | Third-party libraries bundled in-tree. Not maintained as part of Blender's code; style guidelines do not apply here. |
| `scripts/` | Python: startup scripts, UI definitions, add-ons, modules, presets, templates. |
| `lib/` | Platform precompiled libraries / submodules (e.g. `lib/windows_x64`). |
| `tests/` | Test suites: C++ gtests, Python tests, performance tests, test data files. |
| `tools/` | Developer utility scripts (source checks, maintenance, triage, IDE helpers). |
| `build_files/` | CMake modules, build environment, platform/packaging configuration. |
| `release/` | Release datafiles, themes, scripts, platform packaging, release notes. |
| `doc/` | Documentation: blend file format, doxygen config, license info, Python API, man pages. |
| `locale/` | Translations. |
| `assets/` | Bundled assets shipped with Blender. |

---

## `source/` — Application Code

- `source/creator/` — The `main()` entry point and application bootstrap.
- `source/blender/` — All core modules (see below).

### `source/blender/` Modules

| Module | Prefix | Purpose |
|--------|--------|---------|
| `blenlib/` | `BLI_` | Foundational utility library: containers (`Vector`, `Array`, `Set`, `Map`, `Span`), math, strings, memory, threading, file paths. Header-driven. |
| `blenkernel/` | `BKE_` | Core data-block (ID) management and operations: objects, meshes, materials, scenes, modifiers, animation evaluation. The "kernel". |
| `blenloader/` | `BLO_` | Reading/writing `.blend` files, including versioning (`do_version`) for backward/forward compatibility. |
| `blenloader_core/` | | Lower-level blendfile loading primitives. |
| `makesdna/` | `DNA_` | The DNA data model: C structs that define what is serialized to `.blend` files. Drives forward/backward compatibility. See [dna-rna-data-system.md](./dna-rna-data-system.md). |
| `makesrna/` | `RNA_` | The RNA introspection layer wrapping DNA; exposes data to Python and the UI, with get/set callbacks. See [dna-rna-data-system.md](./dna-rna-data-system.md). |
| `bmesh/` | `BM_` | The B-Mesh: editable non-manifold mesh data structure used in Edit Mode and mesh operators. |
| `editors/` | `ED_` | All interactive editors and UI spaces (see breakdown below). |
| `windowmanager/` | `WM_` | Window manager: events, operators, keymaps, the application lifecycle. See [depsgraph-and-operators.md](./depsgraph-and-operators.md). |
| `gpu/` | `GPU_` | GPU abstraction layer over OpenGL / Vulkan / Metal backends; shaders, batches, framebuffers. See [gpu-draw-and-nodes.md](./gpu-draw-and-nodes.md). |
| `draw/` | `DRW_` | The Draw Manager and render engines (EEVEE, Workbench, overlays, selection). See [gpu-draw-and-nodes.md](./gpu-draw-and-nodes.md). |
| `nodes/` | `NOD_` | Node systems: geometry, compositor, shader, function, texture nodes and their evaluation. See [gpu-draw-and-nodes.md](./gpu-draw-and-nodes.md). |
| `geometry/` | `GEO_` | Geometry processing algorithms used by geometry nodes and tools. |
| `functions/` | `FN_` | Multi-function system: vectorized, lazy function evaluation backing nodes. |
| `depsgraph/` | `DEG_` | Dependency graph: tracks relationships between data-blocks and drives evaluation order. See [depsgraph-and-operators.md](./depsgraph-and-operators.md). |
| `modifiers/` | `MOD_` | Mesh/object modifier stack implementations. |
| `animrig/` | `ANIM_` | Animation rigging core (actions, keyframing, drivers — modern C++ layer). |
| `render/` | `RE_` | Render pipeline orchestration (separate from the Cycles engine in `intern/`). |
| `compositor/` | `COM_` | Compositing engine. |
| `sequencer/` | `SEQ_` | Video sequence editor backend. |
| `imbuf/` | `IMB_` | Image buffer: image/movie loading, saving, and pixel operations. |
| `blenfont/` | `BLF_` | Font rendering. |
| `blentranslation/` | `BLT_` | Internationalization / translation layer. |
| `io/` | | Import/export: see breakdown below. |
| `python/` | `BPY_` | The `bpy` Python binding: exposing RNA/operators to Python, the console, the API. See [python-bpy-integration.md](./python-bpy-integration.md). |
| `simulation/` | `SIM_` | Simulation framework. |
| `shader_fx/` | `FX_` | Shader-based visual effects (for Grease Pencil). |
| `freestyle/` | | Freestyle non-photorealistic line renderer. |
| `ikplugin/` | | Inverse kinematics plugin glue (wraps `intern/iksolver`, `intern/itasc`). |
| `asset_system/` | `AS_` | Asset library / asset browser backend. |
| `datatoc/` | | Build tool: converts data files (icons, fonts) into C source. |
| `blendthumb/` | | Thumbnail generator for `.blend` files. |
| `cpucheck/` | | CPU feature detection at startup. |

> **Grease Pencil** (2D drawing — legacy and v3 systems) is not a top-level
> `source/blender/` module. Its data-block code lives in `blenkernel/`
> (`grease_pencil.*`, `BKE_grease_pencil.hh`) and its editor/operator code in
> `editors/grease_pencil/` (v3) and `editors/gpencil_legacy/`.

### `source/blender/editors/` Spaces & Tools

Editor "spaces" (UI areas) and the operators that drive them:

- **Spaces**: `space_view3d/`, `space_node/`, `space_outliner/`, `space_image/`,
  `space_sequencer/`, `space_graph/`, `space_nla/`, `space_action/`,
  `space_buttons/` (properties), `space_file/`, `space_text/`, `space_console/`,
  `space_clip/` (movie clip / tracking), `space_spreadsheet/`, `space_info/`,
  `space_userpref/`, `space_topbar/`, `space_statusbar/`, `space_script/`,
  `space_project/`, `space_api/`.
- **Tools / data domains**: `mesh/`, `curve/`, `curves/`, `object/`,
  `sculpt_paint/` (see [paint-mode-system.md](./paint-mode-system.md)), `armature/`, `animation/`, `physics/`, `transform/`,
  `uvedit/`, `mask/`, `metaball/`, `lattice/`, `pointcloud/`, `geometry/`,
  `grease_pencil/`, `gpencil_legacy/`, `gizmo_library/`, `asset/`,
  `id_management/`, `screen/`, `undo/`, `render/`, `scene/`, `sound/`, `io/`.
- **Shared**: `include/` (the `ED_*` headers), `interface/` (the UI toolkit:
  buttons, layouts, widgets, themes), `util/`, `datafiles/`.

### `source/blender/io/` Import/Export

`alembic/`, `usd/`, `fbx/`, `wavefront_obj/`, `ply/`, `stl/`, `csv/`,
`grease_pencil/`, plus `common/` shared infrastructure.

### `source/blender/nodes/` Node Trees

`geometry/`, `composite/`, `shader/`, `function/`, `texture/`, with shared
infrastructure in `intern/`. Public headers (`NOD_*.hh`) define node
declarations, sockets, evaluation, bundles, closures, and the lazy-function
backing for geometry nodes.

---

## `intern/` — Internal Libraries

| Path | Purpose |
|------|---------|
| `cycles/` | The Cycles path-tracing renderer (CPU + GPU: CUDA/OptiX/HIP/Metal/oneAPI). |
| `ghost/` | GHOST: cross-platform windowing, input, and OpenGL/Vulkan/Metal context handling. |
| `guardedalloc/` | `MEM_` allocator: guarded/tracked memory allocation used throughout Blender. |
| `atomic/` | Atomic operation primitives. |
| `clog/` | C logging library. |
| `memutil/` | Memory utilities (C++). |
| `opensubdiv/` | OpenSubdiv integration (subdivision surfaces). |
| `openvdb/` | OpenVDB integration (volumes). |
| `mantaflow/` | Fluid/smoke simulation (Mantaflow). |
| `rigidbody/` | Rigid body simulation (Bullet glue). |
| `iksolver/`, `itasc/` | Inverse kinematics solvers. |
| `mikktspace/` | Tangent space generation. |
| `eigen/` | Eigen linear algebra wrappers. |
| `libmv/` | Motion tracking / structure-from-motion library. |
| `quadriflow/` | Quad remeshing. |
| `dualcon/` | Dual contouring remesh. |
| `slim/` | SLIM UV unwrapping. |
| `sky/` | Sky model. |
| `draco_bridge/` | Draco mesh compression bridge (glTF). |
| `meshoptimizer_bridge/` | meshoptimizer bridge. |
| `renderdoc_dynload/`, `wayland_dynload/` | Dynamic loaders. |
| `libc_compat/`, `utfconv/`, `uriconvert/` | Compatibility / conversion helpers. |

---

## `scripts/` — Python Layer

| Path | Purpose |
|------|---------|
| `startup/` | Python that runs at startup: `bl_ui/` (the entire UI definition — panels, menus, headers), `bl_operators/`, `bl_keymaps/`, `nodeitems_builtins`, `keyingsets_builtins`. See [python-bpy-integration.md](./python-bpy-integration.md). |
| `modules/` | Importable Python modules (`bpy_extras`, `rna_info`, `bl_i18n_utils`, etc.). |
| `addons_core/` | Core bundled add-ons (e.g. importers/exporters, node tools). |
| `presets/` | Preset `.py` files (render, keyconfig, interface, etc.). |
| `templates_py/`, `templates_osl/`, `templates_toml/` | New-file templates (Python, OSL shaders, extension manifests). |
| `freestyle/` | Freestyle style modules. |
| `site/` | Site customization. |

---

## Build, Test, and Tooling

| Path | Purpose |
|------|---------|
| `CMakeLists.txt` | Root build configuration. |
| `GNUmakefile`, `make.bat` | Convenience wrappers around CMake (`make`, `make format`, etc.). |
| `build_files/cmake/` | CMake macros, platform configs, dependency finders. |
| `build_files/build_environment/` | Scripts to build the precompiled library dependencies. |
| `build_files/buildbot/` | CI/buildbot configuration. |
| `tests/gtests/` | C++ unit tests (gtest). Note: most `*_test.cc` files live next to the code they test. |
| `tests/python/` | Python-driven functional/regression tests. |
| `tests/performance/` | Performance benchmarking harness. |
| `tests/files/` | Test data (often a submodule). |
| `tools/check_source/` | Source style/lint checks (clang-format, license headers, etc.). |
| `tools/utils_maintenance/` | Maintenance scripts (formatting, header sorting). |
| `tools/triage/` | Bug triage helpers. |
| `.clang-format`, `.clang-tidy`, `.editorconfig` | Formatting/lint configuration. |

See [build-and-testing.md](./build-and-testing.md) for build/test workflow details.

---

## Key Conventions at a Glance

- **Module prefixes**: public C/C++ functions are named `MODULE_object_thing_get()`
  (e.g. `BKE_object_*`, `BLI_*`, `ED_*`, `WM_*`, `GPU_*`, `RNA_*`, `DNA_*`).
- **DNA/RNA split**: `makesdna` defines serialized structs; `makesrna` wraps
  them for Python/UI access. Changing DNA requires versioning code in `blenloader`.
- **Headers as the public interface**: a module's `BKE_*.hh` / `BLI_*.hh`
  headers document and define its API; implementation lives in `intern/`.
- **`.blend` compatibility** is a first-class concern — see the compatibility
  guidelines in [CLAUDE.md](../../CLAUDE.md) before changing DNA.

For the full coding standards, naming rules, and contribution workflow, see
[CLAUDE.md](../../CLAUDE.md).
