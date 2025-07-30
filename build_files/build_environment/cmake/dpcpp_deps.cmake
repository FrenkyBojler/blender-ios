# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# These are build time requirements for dpcpp
# We only have to unpack these dpcpp will build
# them.

ExternalProject_Add(external_vcintrinsics
  URL file://${PACKAGE_DIR}/${VCINTRINSICS_FILE}
  URL_HASH ${VCINTRINSICS_HASH_TYPE}=${VCINTRINSICS_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/vcintrinsics
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

# opencl headers do not have to be unpacked, dpcpp will do it
# but it wouldn't hurt to do it anyway as an opertunity to validate
# the hash is correct.
ExternalProject_Add(external_openclheaders
  URL file://${PACKAGE_DIR}/${OPENCLHEADERS_FILE}
  URL_HASH ${OPENCLHEADERS_HASH_TYPE}=${OPENCLHEADERS_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/openclheaders
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

# icdloader does not have to be unpacked, dpcpp will do it
# but it wouldn't hurt to do it anyway as an opertunity to validate
# the hash is correct.
ExternalProject_Add(external_icdloader
  URL file://${PACKAGE_DIR}/${ICDLOADER_FILE}
  URL_HASH ${ICDLOADER_HASH_TYPE}=${ICDLOADER_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/icdloader
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

ExternalProject_Add(external_emhash
  URL file://${PACKAGE_DIR}/${EMHASH_FILE}
  URL_HASH ${EMHASH_HASH_TYPE}=${EMHASH_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/emhash
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

ExternalProject_Add_Step(external_emhash after_download
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/emhash/src/external_emhash/include/emhash
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/emhash/src/external_emhash/hash_table8.hpp ${BUILD_DIR}/emhash/src/external_emhash/include/emhash
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/emhash/src/external_emhash/hash_table7.hpp ${BUILD_DIR}/emhash/src/external_emhash/include/emhash
  DEPENDEES download
)

ExternalProject_Add(external_dpcpp_spirvheaders
  URL file://${PACKAGE_DIR}/${DPCPP_SPIRV_HEADERS_FILE}
  URL_HASH ${DPCPP_SPIRV_HEADERS_HASH_TYPE}=${DPCPP_SPIRV_HEADERS_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/dpcpp_spirvheaders
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

ExternalProject_Add(external_unifiedruntime
  URL file://${PACKAGE_DIR}/${UNIFIED_RUNTIME_FILE}
  URL_HASH ${UNIFIED_RUNTIME_HASH_TYPE}=${UNIFIED_RUNTIME_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/unifiedruntime
  PATCH_COMMAND ${PATCH_CMD} -p 1 -d
    ${BUILD_DIR}/unifiedruntime/src/external_unifiedruntime <
    ${PATCH_DIR}/unifiedruntime.diff
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

ExternalProject_Add(external_unifiedmemoryframework
  URL file://${PACKAGE_DIR}/${UNIFIED_MEMORY_FRAMEWORK_FILE}
  URL_HASH ${UNIFIED_MEMORY_FRAMEWORK_HASH_TYPE}=${UNIFIED_MEMORY_FRAMEWORK_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/unifiedmemoryframework
  PATCH_COMMAND ${PATCH_CMD} -p 1 -d
    ${BUILD_DIR}/unifiedmemoryframework/src/external_unifiedmemoryframework <
    ${PATCH_DIR}/unifiedmemoryframework.diff
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

ExternalProject_Add(external_parallelhashmap
  URL file://${PACKAGE_DIR}/${PARALLEL_HASHMAP_FILE}
  URL_HASH ${PARALLEL_HASHMAP_HASH_TYPE}=${PARALLEL_HASHMAP_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/parallelhashmap
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

ExternalProject_Add_Step(external_parallelhashmap after_download
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap_fwd_decl.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap_utils.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap_bits.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap_config.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap_base.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  COMMAND ${CMAKE_COMMAND} -E copy ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/parallel_hashmap/phmap_dump.h ${BUILD_DIR}/parallelhashmap/src/external_parallelhashmap/include/parallel_hashmap
  DEPENDEES download
)