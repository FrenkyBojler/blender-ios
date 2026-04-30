# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(MESHOPTIMIZER_EXTRA_ARGS
)

ExternalProject_Add(external_meshoptimizer
  URL file://${PACKAGE_DIR}/${MESHOPTIMIZER_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${MESHOPTIMIZER_HASH_TYPE}=${MESHOPTIMIZER_HASH}
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}
  PREFIX ${BUILD_DIR}/meshoptimizer

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/meshoptimizer
    ${DEFAULT_CMAKE_FLAGS}
    ${MESHOPTIMIZER_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/meshoptimizer
)

ExternalProject_Add_Step(external_meshoptimizer after_install
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${LIBDIR}/meshoptimizer/
    ${HARVEST_TARGET}/meshoptimizer
  DEPENDEES install
)
