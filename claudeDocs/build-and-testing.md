# Build System and Testing

## Build System

Blender's build is CMake-driven; `GNUmakefile` / `make.bat` are thin
convenience wrappers around it.

| Entry point | Purpose |
|---|---|
| `CMakeLists.txt` (root) | Top-level CMake configuration, options, subdirectory includes. |
| `GNUmakefile` | Unix convenience wrapper: `make`, `make deps`, `make test`, `make format`, `make help`. |
| `make.bat` | Windows equivalent (invokes CMake/MSBuild or Ninja). |
| `build_files/cmake/` | CMake macros (`macros.cmake`), platform configs (`platform/`), feature detection (`have_features.cmake`), packaging (`packaging.cmake`), test registration (`testing.cmake`). |
| `build_files/build_environment/` | Scripts to build the precompiled third-party library set (what ends up in `lib/<platform>`, e.g. `lib/windows_x64` in this checkout). |
| `build_files/buildbot/` | CI/buildbot configuration consumed by Blender's official build farm. |
| `build_files/utils/make_test.py` | Script invoked by `make test` — wraps `ctest`. |
| `build_files/utils/make_benchmark.py` | Script invoked by `make benchmark`. |

### Common `make` targets (`GNUmakefile`)

- `make` — configure + build with the default/last-used config.
- `make deps` — build the precompiled dependency libraries (rarely needed;
  most platforms fetch prebuilt `lib/` libraries via SVN/git-lfs instead).
- `make test` — runs `build_files/utils/make_test.py "$(BUILD_DIR)"` (wraps `ctest`).
- `make benchmark` — runs the performance benchmark harness.
- `make format` — runs `clang-format` (C/C++/GLSL) and `autopep8` (Python)
  over changed files; this is the automated formatting referenced throughout
  [CLAUDE.md](../CLAUDE.md).
- `make project_qtcreator` / `make project_eclipse` — generate IDE project files.
- `make check_cppcheck` and related `check_*` targets — static analysis.

### Unity builds

Blender defaults to **unity builds** (`WITH_UNITY_BUILD`), which concatenate
multiple `.cc` files into one translation unit per compile job for speed.
This can **mask missing `#include`s** — a file may compile fine inside a
unity blob because a sibling file in the same blob already pulled in the
header it needs. When refactoring headers or moving code between files,
disable `WITH_UNITY_BUILD` and do a clean build to catch these (see
"Testing Changes & Refactors" in [CLAUDE.md](../CLAUDE.md)).

### Unity-build symbol collisions (C++)

Because unity builds concatenate files, file-private symbols can collide
across files in the same blob. Convention: put compile-unit-private symbols
in `blender::<module>::unity_build_<file>_cc` namespaces to avoid this.

---

## Testing

| Suite | Location | Runner |
|---|---|---|
| C++ unit tests (gtest) | `tests/gtests/`, plus most `*_test.cc` files colocated next to the code they test | `ctest` (via `make test`) |
| Python functional/regression tests | `tests/python/` | `ctest` targets that invoke Blender with test scripts |
| Performance benchmarks | `tests/performance/` | `make benchmark` |
| Test data | `tests/files/` | often a separate submodule, fetched via `svn`/git-lfs |

### Practical guidance (from [CLAUDE.md](../CLAUDE.md))

- Run automated tests locally during development — buildbot is the *last*
  stage of verification, not the first.
- Debug + ASAN builds catch more bugs but are slow; skip Cycles tests there
  (`ctest -E cycles`).
- Manual testing is mandatory for significant changes: open complex
  production files, look for regressions, craft small demo cases, round-trip
  files through older Blender releases to check compatibility.
- `@blender-bot build` (comment on a PR) asks the buildbot to build/test a
  PR — needed because a local build succeeding doesn't guarantee others'
  platforms will build cleanly.
- `@blender-bot package` shares a temporary build with testers.

---

## Source Style Checks (`tools/check_source/`)

Automated lint/style checks distinct from `clang-format`/`autopep8`:
license header presence (SPDX), formatting conformance, and other
repository-wide conventions. `tools/utils_maintenance/` holds one-off
maintenance scripts (bulk formatting fixes, header sorting).

---

See [project-overview.md](./project-overview.md) for the repository map and
[CLAUDE.md](../CLAUDE.md) for the full coding-standards and committer
guidelines this build/test workflow supports.
