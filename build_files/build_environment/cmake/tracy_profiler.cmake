# SPDX-FileCopyrightText: 2002-2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# XXX: Avoid downloads done by CPM
# XXX: Avoid /opt/homebrew/opt/glfw/lib/libglfw.3.dylib
# XXX: Avoid /opt/homebrew/opt/freetype/lib/libfreetype.6.dylib

set(TRACY_PROFILER_EXTRA_ARGS
  -DGIT_REV=${TRACY_VERSION}
)

ExternalProject_Add(external_tracy_profiler
  URL file://${PACKAGE_DIR}/${TRACY_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${TRACY_HASH_TYPE}=${TRACY_HASH}
  PREFIX ${BUILD_DIR}/tracy_profiler
  SOURCE_SUBDIR profiler

  PATCH_COMMAND
    ${PATCH_CMD} -p 1 -d
      ${BUILD_DIR}/tracy_profiler/src/external_tracy_profiler <
      ${PATCH_DIR}/tracy_profiler_git_ref.diff

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/tracy/profiler
    ${DEFAULT_CMAKE_FLAGS}
    ${TRACY_PROFILER_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/tracy/profiler
)
if(WIN32)
  if(BUILD_MODE STREQUAL Release)
    ExternalProject_Add_Step(external_tracy_profiler after_install
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/tracy/profiler
        ${HARVEST_TARGET}/tracy/profiler

      DEPENDEES install
    )
  endif()
else()
  harvest(external_tracy_profiler tracy/profiler/bin tracy/profiler/bin "tracy-profiler")
endif()
