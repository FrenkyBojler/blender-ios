# SPDX-FileCopyrightText: 2016 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  src/collider/include
  src/manifold/include
  src/polygon/include
  src/utilities/include
)

set(INC_SYS
  src/third_party/glm
  src/third_party/thrust
  src/third_party/thrust/dependencies/libcudacxx/include
)

if (WITH_TBB)
  list(APPEND INC_SYS ${TBB_INCLUDE_DIRS})
  add_definitions(
    -DMANIFOLD_PAR='T'
    -DTHRUST_DEVICE_SYSTEM=THRUST_DEVICE_SYSTEM_TBB
  )
  if (WIN32)
    add_definitions(-DNOMINMAX)
  endif()
else()
  add_definitions(
    -DMANIFOLD_PAR='C'
    -DTHRUST_DEVICE_SYSTEM=THRUST_DEVICE_SYSTEM_CPP
  )
endif()

set(SRC
  src/collider/src/collider.cpp
  src/manifold/src/boolean_result.cpp
  src/manifold/src/boolean3.cpp
  src/manifold/src/constructors.cpp
  src/manifold/src/csg_tree.cpp
  src/manifold/src/edge_op.cpp
  src/manifold/src/face_op.cpp
  src/manifold/src/impl.cpp
  src/manifold/src/manifold.cpp
  src/manifold/src/properties.cpp
  src/manifold/src/smoothing.cpp
  src/manifold/src/sort.cpp
  src/polygon/src/polygon.cpp

  src/collider/include/collider.h
  src/manifold/include/manifold.h
  src/manifold/src/boolean3.h
  src/manifold/src/csg_tree.h
  src/manifold/src/impl.h
  src/manifold/src/mesh_fixes.h
  src/manifold/src/shared.h
  src/polygon/include/polygon.h
  src/utilities/include/hashtable.h
  src/utilities/include/optional_assert.h
  src/utilities/include/par.h
  src/utilities/include/public.h
  src/utilities/include/sparse.h
  src/utilities/include/svd.h
  src/utilities/include/utils.h
  src/utilities/include/vec_view.h
  src/utilities/include/vec.h
)

FILE(GLOB glm_headers
  src/third_party/glm/glm/*.hpp
  src/third_party/glm/glm/*/*.hpp
  src/third_party/glm/glm/*/*.inl
)

FILE(GLOB thrust_headers
  src/third_party/thrust/thrust/*.h
  src/third_party/thrust/thrust/*.inl
  src/third_party/thrust/thrust/*/*.h
  src/third_party/thrust/thrust/*/*.inl
  src/third_party/thrust/thrust/*/*/*.h
  src/third_party/thrust/thrust/*/*/*.inl
  src/third_party/thrust/thrust/*/*/*/*.h
  src/third_party/thrust/thrust/*/*/*/*.inl
)

LIST(APPEND SRC ${glm_headers} ${thrust_headers})

if (MSVC)
else()
  add_definitions("-O3")
endif()

set(LIB
)

blender_add_lib(extern_manifold "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
