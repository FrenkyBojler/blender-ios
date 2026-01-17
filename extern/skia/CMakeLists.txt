# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# -----------------------------------------------------------------------------
# Skia PathOps wrapper for Blender's overlap removal. Provides an alternative
# to FontForge using Skia's Simplify function. This builds a minimal subset of
# Skia containing only the PathOps module and its required dependencies.

set(INC
  .
  intern
  intern/include
  intern/include/config
)

set(INC_SYS
)

# -----------------------------------------------------------------------------
# PathOps module sources.

set(PATHOPS_SRC
  intern/src/pathops/SkAddIntersections.cpp
  intern/src/pathops/SkDConicLineIntersection.cpp
  intern/src/pathops/SkDCubicLineIntersection.cpp
  intern/src/pathops/SkDCubicToQuads.cpp
  intern/src/pathops/SkDLineIntersection.cpp
  intern/src/pathops/SkDQuadLineIntersection.cpp
  intern/src/pathops/SkIntersections.cpp
  intern/src/pathops/SkOpAngle.cpp
  intern/src/pathops/SkOpBuilder.cpp
  intern/src/pathops/SkOpCoincidence.cpp
  intern/src/pathops/SkOpContour.cpp
  intern/src/pathops/SkOpCubicHull.cpp
  intern/src/pathops/SkOpEdgeBuilder.cpp
  intern/src/pathops/SkOpSegment.cpp
  intern/src/pathops/SkOpSpan.cpp
  intern/src/pathops/SkPathOpsAsWinding.cpp
  intern/src/pathops/SkPathOpsCommon.cpp
  intern/src/pathops/SkPathOpsConic.cpp
  intern/src/pathops/SkPathOpsCubic.cpp
  intern/src/pathops/SkPathOpsCurve.cpp
  intern/src/pathops/SkPathOpsDebug.cpp
  intern/src/pathops/SkPathOpsLine.cpp
  intern/src/pathops/SkPathOpsOp.cpp
  intern/src/pathops/SkPathOpsQuad.cpp
  intern/src/pathops/SkPathOpsRect.cpp
  intern/src/pathops/SkPathOpsSimplify.cpp
  intern/src/pathops/SkPathOpsTightBounds.cpp
  intern/src/pathops/SkPathOpsTSect.cpp
  intern/src/pathops/SkPathOpsTypes.cpp
  intern/src/pathops/SkPathOpsWinding.cpp
  intern/src/pathops/SkPathWriter.cpp
  intern/src/pathops/SkReduceOrder.cpp
)

# -----------------------------------------------------------------------------
# Core module sources (minimal subset needed by PathOps).

set(CORE_SRC
  intern/src/core/SkPath.cpp
  intern/src/core/SkPathBuilder.cpp
  intern/src/core/SkPathData.cpp
  intern/src/core/SkPathDump.cpp
  intern/src/core/SkPathIter.cpp
  intern/src/core/SkPathPriv.cpp
  intern/src/core/SkPathRaw.cpp
  intern/src/core/SkPathRawShapes.cpp
  intern/src/core/SkPath_pathdata.cpp
  intern/src/core/SkGeometry.cpp
  intern/src/core/SkIDChangeListener.cpp
  intern/src/core/SkPoint.cpp
  intern/src/core/SkRect.cpp
  intern/src/core/SkMatrix.cpp
  intern/src/core/SkRRect.cpp
  intern/src/core/SkString.cpp
  intern/src/core/SkStringUtils.cpp
  intern/src/core/SkCubicClipper.cpp
  intern/src/core/SkEdgeClipper.cpp
  intern/src/core/SkLineClipper.cpp
  intern/src/core/SkM44.cpp
  intern/src/core/SkMatrixInvert.cpp
)

# -----------------------------------------------------------------------------
# Base module sources (minimal subset needed by Core and PathOps).

set(BASE_SRC
  intern/src/base/SkArenaAlloc.cpp
  intern/src/base/SkBezierCurves.cpp
  intern/src/base/SkBlockAllocator.cpp
  intern/src/base/SkBuffer.cpp
  intern/src/base/SkContainers.cpp
  intern/src/base/SkCubics.cpp
  intern/src/base/SkDebug.cpp
  intern/src/base/SkDeque.cpp
  intern/src/base/SkFloatingPoint.cpp
  intern/src/base/SkHalf.cpp
  intern/src/base/SkMalloc.cpp
  intern/src/base/SkMathPriv.cpp
  intern/src/base/SkQuads.cpp
  intern/src/base/SkSafeMath.cpp
  intern/src/base/SkSemaphore.cpp
  intern/src/base/SkSpinlock.cpp
  intern/src/base/SkTDArray.cpp
  intern/src/base/SkTSearch.cpp
  intern/src/base/SkThreadID.cpp
  intern/src/base/SkUTF.cpp
  intern/src/base/SkUtils.cpp
)

# -----------------------------------------------------------------------------
# Platform-specific sources.

if(WIN32)
  list(APPEND BASE_SRC
    intern/src/ports/SkDebug_win.cpp
    intern/src/ports/SkOSFile_win.cpp
  )
  list(APPEND CORE_SRC
    intern/src/ports/SkMemory_malloc.cpp
  )
else()
  list(APPEND BASE_SRC
    intern/src/ports/SkDebug_stdio.cpp
    intern/src/ports/SkOSFile_posix.cpp
    intern/src/ports/SkOSFile_stdio.cpp
  )
  list(APPEND CORE_SRC
    intern/src/ports/SkMemory_malloc.cpp
  )
endif()

# -----------------------------------------------------------------------------
# Blender wrapper.

set(WRAPPER_SRC
  skpathops_blender.cc
  skpathops_blender.h
)

# Combine all sources.
set(SRC
  ${WRAPPER_SRC}
  ${PATHOPS_SRC}
  ${CORE_SRC}
  ${BASE_SRC}
)

set(LIB
)

# Skia requires C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Silence some warnings in Skia code.
if(CMAKE_COMPILER_IS_GNUCC)
  add_cxx_flag(
    "-Wno-class-memaccess"
    "-Wno-maybe-uninitialized"
  )
endif()
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
  add_cxx_flag(
    "-Wno-unused-parameter"
    "-Wno-sign-compare"
    "-Wno-implicit-fallthrough"
  )
  remove_cc_flag(
    "-Wmissing-declarations"
  )
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  add_cxx_flag(
    "-Wno-unused-private-field"
  )
endif()

add_definitions(
  # Enable actual Skia PathOps implementation.
  -DWITH_SKIA_PATHOPS
  # Disable features we don't need.
  -DSK_DISABLE_LEGACY_SKUNICHAR
  -DSK_DISABLE_TRACING
)

# Platform-specific definitions.
if(WIN32)
  add_definitions(
    -DSK_BUILD_FOR_WIN
    -DNOMINMAX
  )
elseif(APPLE)
  add_definitions(-DSK_BUILD_FOR_MAC)
elseif(UNIX)
  add_definitions(-DSK_BUILD_FOR_UNIX)
endif()

blender_add_lib(extern_skia "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::extern::skia ALIAS extern_skia)
