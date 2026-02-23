# SPDX-FileCopyrightText: 2026 Blender Foundation, The Khronos Group
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Build meshoptimizer library.
add_subdirectory(meshopt)

set(INC
  meshopt/src
)

# Build meshoptimizer-Blender bridging module.
set(SRC
  src/decoder.cpp
  src/common.h
  src/decoder.h
)

set(LIB
  meshoptimizer
)

add_library(extern_meshoptimizer SHARED "${SRC}")
target_include_directories(extern_meshoptimizer PUBLIC "${INC}")
target_link_libraries(extern_meshoptimizer PUBLIC "${LIB}")

blender_source_group(extern_meshoptimizer "${SRC}")

