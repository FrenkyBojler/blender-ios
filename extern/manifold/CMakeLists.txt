# SPDX-FileCopyrightText: 2016 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  include
)

if (WITH_TBB)
  list(APPEND INC_SYS ${TBB_INCLUDE_DIRS})
  add_definitions(
    -DMANIFOLD_PAR=1
  )
  if (WIN32)
    add_definitions(-DNOMINMAX)
  endif()
else()
  add_definitions(
    -DMANIFOLD_PAR=-1
  )
endif()

set(SRC
  src/boolean3.cpp
  src/boolean_result.cpp
  src/constructors.cpp
  src/csg_tree.cpp
  src/edge_op.cpp
  src/face_op.cpp
  src/impl.cpp
  src/manifold.cpp
  src/polygon.cpp
  src/properties.cpp
  src/quickhull.cpp
  src/sdf.cpp
  src/smoothing.cpp
  src/sort.cpp
  src/subdivision.cpp

  src/boolean3.h
  src/collider.h
  src/csg_tree.h
  src/hashtable.h
  src/impl.h
  src/iters.h
  src/mesh_fixes.h
  src/parallel.h
  src/quickhull.h
  src/shared.h
  src/sparse.h
  src/svd.h
  src/tri_dist.h
  src/utils.h
  src/vec.h
  include/manifold/common.h
  include/manifold/linalg.h
  include/manifold/manifold.h
  include/manifold/optional_assert.h
  include/manifold/vec_view.h
)

if (MSVC)
else()
  add_definitions("-O3")
endif()

set(LIB
)

blender_add_lib(extern_manifold "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
