# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Quickhull just needs to be unpacked so the manifold build can copy it
# to the right spot in their source tree
ExternalProject_Add(external_quickhull
  URL file://${PACKAGE_DIR}/${QUICKHULL_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${QUICKHULL_HASH_TYPE}=${QUICKHULL_HASH}
  PREFIX ${BUILD_DIR}/quickhull
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)

set(MANIFOLD_EXTRA_ARGS
  -DMANIFOLD_TEST=OFF
  -Dglm_DIR=${LIBDIR}/glm/share/glm/
  -Dtbb_DIR=${LIBDIR}/tbb/lib/cmake/tbb/
  -DThrust_DIR=${LIBDIR}/cccl/lib/cmake/thrust/
  -Dlibcudacxx_DIR=${LIBDIR}/cccl/lib/cmake/libcudacxx/
  -DClipper2_DIR=${LIBDIR}/clipper2/lib/cmake/clipper2/
  -DMANIFOLD_CBIND=OFF
  -DCMAKE_INSTALL_DATADIR=lib
  -DCMAKE_INSTALL_INCLUDEDIR=include
  -DMANIFOLD_PAR=TBB
  -DCMAKE_DEBUG_POSTFIX=_d
  -DBUILD_SHARED_LIBS=OFF
)

ExternalProject_Add(external_manifold
  URL file://${PACKAGE_DIR}/${MANIFOLD_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${MANIFOLD_HASH_TYPE}=${MANIFOLD_HASH}
  PREFIX ${BUILD_DIR}/manifold
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}
  
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/manifold
    ${DEFAULT_CMAKE_FLAGS}
    ${MANIFOLD_EXTRA_ARGS}

  PATCH_COMMAND
    ${CMAKE_COMMAND} -E copy_directory
      ${BUILD_DIR}/quickhull/src/external_quickhull
      ${BUILD_DIR}/manifold/src/external_manifold/src/third_party/quickhull &&
    ${PATCH_CMD} -p 1 -N -d
      ${BUILD_DIR}/manifold/src/external_manifold/ <
      ${PATCH_DIR}/manifold.diff

  INSTALL_DIR ${LIBDIR}/manifold
)

add_dependencies(
  external_manifold
  external_tbb
  external_quickhull
  external_clipper2
  external_cccl
  external_glm
)
