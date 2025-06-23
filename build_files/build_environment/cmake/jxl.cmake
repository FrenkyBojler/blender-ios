# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(JXL_EXTRA_ARGS
  -DBUILD_TESTING=OFF
  -DJPEGXL_ENABLE_SKCMS=ON
  -DJPEGXL_ENABLE_SJPEG=OFF
  -DJPEGXL_ENABLE_OPENEXR=OFF
  -DJPEGXL_ENABLE_TOOLS=OFF
  -DBUILD_SHARED_LIBS=ON
  -DHWY_INCLUDE_DIR=${LIBDIR}/highway/include
  -DHWY_LIBRARY=${LIBDIR}/highway/lib/hwy${LIBEXT}
  -DBROTLI_INCLUDE_DIR=${LIBDIR}/brotli/include
  -DBROTLICOMMON_LIBRARY=${LIBDIR}/brotli/lib/brotlicommon-static${LIBEXT}
  -DBROTLIENC_LIBRARY=${LIBDIR}/brotli/lib/brotlienc-static${LIBEXT}
  -DBROTLIDEC_LIBRARY=${LIBDIR}/brotli/lib/brotlidec-static${LIBEXT}
)

ExternalProject_Add(external_jxl
  URL file://${PACKAGE_DIR}/${JXL_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${JXL_HASH_TYPE}=${JXL_HASH}
  PREFIX ${BUILD_DIR}/jxl
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${LIBDIR}/jxl ${DEFAULT_CMAKE_FLAGS} ${JXL_EXTRA_ARGS}
  INSTALL_DIR ${LIBDIR}/jxl
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}
  
  PATCH_COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${BUILD_DIR}/skcms/src/external_skcms/
        ${BUILD_DIR}/jxl/src/external_jxl/third_party/skcms/
)

add_dependencies(
  external_jxl
  external_highway
  external_brotli
  external_skcms
)

if(WIN32)
  if(BUILD_MODE STREQUAL Release)
    ExternalProject_Add_Step(external_jxl after_install
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/jxl/
        ${HARVEST_TARGET}/jxl/
        DEPENDEES install
    )
  endif()
endif()
