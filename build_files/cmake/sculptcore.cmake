# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# SculptCore integration: build the external engine (extern/sculptcore) and
# vendor its runtime (ctypes package + native DLL) into the sculptcore_addon,
# so the installed Blender ships a working sculpt-mode addon.
#
# The engine is a self-contained CMake+Node project that does not compile into
# blender.exe; a small Node dispatcher (`make.mjs bundle`) builds the shared
# library and stages the addon runtime. This wires that into Blender's build so
# `cmake --build` produces a complete Blender + SculptCore, and build farms need
# no extra manual step.
#
# Expects `TARGETDIR_VER` to be set by the including scope.

find_program(SCULPTCORE_NODE_EXECUTABLE NAMES node nodejs)
if(NOT SCULPTCORE_NODE_EXECUTABLE)
  message(FATAL_ERROR
    "WITH_SCULPTCORE is enabled but Node.js was not found on PATH. "
    "Install Node.js (https://nodejs.org), or configure with -DWITH_SCULPTCORE=OFF."
  )
endif()

set(_sculptcore_root ${CMAKE_SOURCE_DIR}/extern/sculptcore)
set(_sculptcore_stage ${CMAKE_BINARY_DIR}/sculptcore_stage)
set(_sculptcore_deps_publish ${CMAKE_BINARY_DIR}/sculptcore_deps_publish)

# Build the engine (and its native deps) in the same config as Blender.
# `$<CONFIG>` resolves for both single- and multi-config generators.
set(_sculptcore_config "$<CONFIG>")

# Always built: `make.mjs bundle` is incremental (a no-op rebuild is cheap), so
# this stays inexpensive on unchanged trees while guaranteeing the staged addon
# runtime is current. `make_directory` keeps both stage roots present even when
# nothing is produced (e.g. a deps cache hit stages no deps), so the install
# rules below never reference a missing directory.
add_custom_target(
  bf_sculptcore_bundle ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_sculptcore_stage}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_sculptcore_deps_publish}
  COMMAND ${CMAKE_COMMAND} -E env "SCULPTCORE_CMAKE_BUILD_TYPE=${_sculptcore_config}"
    ${SCULPTCORE_NODE_EXECUTABLE} make.mjs bundle ${_sculptcore_stage}
      --publish-deps-to ${_sculptcore_deps_publish}
  WORKING_DIRECTORY ${_sculptcore_root}
  COMMENT "Building SculptCore engine and bundling the addon runtime"
  USES_TERMINAL
  VERBATIM
)

# The bundled ctypes package + native DLL, vendored into the addon's lib/. This
# runs after the generic `scripts` install (declaration order), so it overwrites
# any stale source-tree lib/ copied there with the freshly staged runtime.
install(
  DIRECTORY ${_sculptcore_stage}/sculptcore/
  DESTINATION ${TARGETDIR_VER}/scripts/addons_core/sculptcore_addon/lib/sculptcore
)

# Freshly-built native deps (OpenBLAS + SuiteSparse/CHOLMOD), present only when
# this build compiled them from source (a cache miss). They are statically
# linked into the DLL and must NOT ship in the Blender package, so they go to a
# separate, opt-in install component that the default install skips. Collect
# them as their own artifact with:
#   cmake --install <build> --component sculptcore_deps --prefix <deps-out>
# (e.g. a second buildbot upload step), then publish them to the sculptcore-deps
# repo with extern/sculptcore/tools/publish-deps-from-package.mjs.
install(
  DIRECTORY ${_sculptcore_deps_publish}/
  DESTINATION sculptcore-deps
  COMPONENT sculptcore_deps
  EXCLUDE_FROM_ALL
)
