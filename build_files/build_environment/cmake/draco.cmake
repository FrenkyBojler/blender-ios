# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(DRACO_EXTRA_ARGS
)

if(WIN32)
  set(DRACO_EXTRA_ARGS
    ${DRACO_EXTRA_ARGS}
    -DCMAKE_DEBUG_POSTFIX=_d
  )
endif()

ExternalProject_Add(external_draco
  URL file://${PACKAGE_DIR}/${DRACO_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${DRACO_HASH_TYPE}=${DRACO_HASH}
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}
  PREFIX ${BUILD_DIR}/draco

  CMAKE_ARGS
  -DCMAKE_INSTALL_PREFIX=${LIBDIR}/draco
  ${DEFAULT_CMAKE_FLAGS}
  ${DRACO_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/draco
)

ExternalProject_Add_Step(external_draco after_install
  COMMAND ${CMAKE_COMMAND} -E copy_directory
  ${LIBDIR}/draco/
  ${HARVEST_TARGET}/draco
  DEPENDEES install
)
