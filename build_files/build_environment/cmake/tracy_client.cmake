# SPDX-FileCopyrightText: 2002-2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(TRACY_CLIENT_EXTRA_ARGS
)

ExternalProject_Add(external_tracy_client
  URL file://${PACKAGE_DIR}/${TRACY_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${TRACY_HASH_TYPE}=${TRACY_HASH}
  PREFIX ${BUILD_DIR}/tracy_client

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/tracy/client
    ${DEFAULT_CMAKE_FLAGS}
    ${TRACY_CLIENT_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/tracy/client
)
if(WIN32)
  if(BUILD_MODE STREQUAL Release)
    ExternalProject_Add_Step(external_tracy_client after_install
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/tracy/client
        ${HARVEST_TARGET}/tracy/client

      DEPENDEES install
    )
  endif()
  if(BUILD_MODE STREQUAL Debug)
    ExternalProject_Add_Step(external_tracy_client after_install
      COMMAND ${CMAKE_COMMAND} -E copy
        ${LIBDIR}/tracy/client/lib/TracyClient.lib
        ${HARVEST_TARGET}/tracy/client/lib/TracyClient_d.lib

      DEPENDEES install
    )
  endif()
else()
  harvest(external_tracy_client tracy/client/include tracy/client/include "*")
  harvest(external_tracy_client tracy/client/lib tracy/client/lib "*.a")
endif()
