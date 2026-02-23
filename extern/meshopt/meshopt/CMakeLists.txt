# SPDX-FileCopyrightText: 2026 Blender Foundation, The Khronos Group
#
# SPDX-License-Identifier: GPL-2.0-or-later

remove_strict_flags()

set(INC
  src
)

set(SRC

    src/meshoptimizer
    src/meshoptimizer/allocator.cpp
    src/meshoptimizer/clusterizer.cpp
    src/meshoptimizer/indexanalyzer.cpp
    src/meshoptimizer/indexcodec.cpp
    src/meshoptimizer/indexgenerator.cpp
    src/meshoptimizer/meshoptimizer.h
    src/meshoptimizer/overdrawoptimizer.cpp
    src/meshoptimizer/partition.cpp
    src/meshoptimizer/quantization.cpp
    src/meshoptimizer/rasterizer.cpp
    src/meshoptimizer/simplifier.cpp
    src/meshoptimizer/spatialorder.cpp
    src/meshoptimizer/stripifier.cpp
    src/meshoptimizer/vcacheoptimizer.cpp
    src/meshoptimizer/vertexcodec.cpp
    src/meshoptimizer/vertexfilter.cpp
    src/meshoptimizer/vfetchoptimizer.cpp

)

set(LIB
)

blender_add_lib(meshoptimizer "${SRC}" "${INC}" "" "${LIB}")
