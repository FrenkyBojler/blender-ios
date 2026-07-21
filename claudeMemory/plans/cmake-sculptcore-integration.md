# Plan — main CMake build drives SculptCore build + bundle

Goal: `cmake --build` of Blender (with `WITH_SCULPTCORE=ON`, default ON)
builds `sculptcore_capi.dll`, vendors the ctypes package + DLLs into the
installed addon, and enables the addon by default. Freshly-built native deps
(OpenBLAS + SuiteSparse/CHOLMOD) are captured for publishing back to the
`sculptcore-deps` repo without shipping in the final package.

## Decisions (agreed with user)
- `WITH_SCULPTCORE` default **ON**; a CMake custom target builds the DLL.
- Auto-enable: add `sculptcore_addon` to `_addons_hidden_core` (always-on,
  persistent, not user-toggleable), gated on `bpy.app.build_options.sculptcore`.
- Deps: **separate `sculptcore_deps` install COMPONENT** is the source of
  truth (no package bloat); a companion off-farm tool can also recover the
  combo from wherever it lands, commit+push to `sculptcore-deps`, and strip it.
  Buildbot supports multiple upload steps, so a second artifact is clean.

## Pieces
1. `CMakeLists.txt`: `option(WITH_SCULPTCORE ... ON)` + `info_cfg_option`.
2. `source/blender/python/intern/CMakeLists.txt`: `add_definitions(-DWITH_SCULPTCORE)`.
3. `bpy_app_build_options.cc`: `sculptcore` field + `#ifdef` block (keep order).
4. `extern/sculptcore/make.mjs`:
   - `SCULPTCORE_CMAKE_BUILD_TYPE` env override for `CMAKE_BUILD_TYPE`.
   - `bundle --publish-deps-to <dir>`: after build, if deps were freshly built
     (cache miss), copy the combo into `<dir>/<platform>/<toolchain>/<config>/`.
5. `extern/sculptcore/tools/deps.mjs`: `ensureDeps` writes
   `build/deps-last-fresh.json` = `{comboRel, comboDir, fresh}` each run.
6. `build_files/cmake/sculptcore.cmake` (included from creator CMakeLists under
   the `WITH_PYTHON` install block):
   - `find_program(NODE_EXECUTABLE node)`, hard-error if absent.
   - `add_custom_target(bf_sculptcore_bundle ALL ...)` → stages into
     `${CMAKE_BINARY_DIR}/sculptcore_stage/sculptcore` and
     `${CMAKE_BINARY_DIR}/sculptcore_deps_publish`.
   - `install(DIRECTORY .../sculptcore_stage/sculptcore/ DESTINATION
     .../addons_core/sculptcore_addon/lib/sculptcore)`.
   - `install(DIRECTORY .../sculptcore_deps_publish/ DESTINATION sculptcore-deps
     OPTIONAL COMPONENT sculptcore_deps EXCLUDE_FROM_ALL)`.
7. `scripts/modules/addon_utils.py`: gate hidden-core add on build option.
8. `extern/sculptcore/tools/publish-deps-from-package.mjs`: recover a combo
   from a dir/zip, commit+push to sculptcore-deps (git-lfs), strip it.
9. CLAUDE.md cleanup checklist: note these edits to revert before the PR.
