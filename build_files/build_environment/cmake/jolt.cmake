# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# TODO example cmake script: https://github.com/jrouwe/JoltPhysicsHelloWorld/blob/main/Build/CMakeLists.txt

# When turning this option on, the library will be compiled using doubles for positions. This allows for much bigger worlds.
set(DOUBLE_PRECISION OFF)

# When turning this option on, the library will be compiled with debug symbols
set(GENERATE_DEBUG_SYMBOLS ON)

# When turning this option on, the library will be compiled in such a way to attempt to keep the simulation deterministic across platforms
set(CROSS_PLATFORM_DETERMINISTIC ON)

# When turning this option on, the library will be compiled with interprocedural optimizations enabled, also known as link-time optimizations or link-time code generation.
# Note that if you turn this on you need to use SET_INTERPROCEDURAL_OPTIMIZATION() or set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON) to enable LTO specificly for your own project as well.
# If you don't do this you may get an error: /usr/bin/ld: libJolt.a: error adding symbols: file format not recognized
set(INTERPROCEDURAL_OPTIMIZATION OFF)

# When turning this on, in Debug and Release mode, the library will emit extra code to ensure that the 4th component of a 3-vector is kept the same as the 3rd component 
# and will enable floating point exceptions during simulation to detect divisions by zero. 
# Note that this currently only works using MSVC. Clang turns Float2 into a SIMD vector sometimes causing floating point exceptions (the option is ignored).
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
set(CPP_EXCEPTIONS_ENABLED ON)

# Number of bits to use in ObjectLayer. Can be 16 or 32.
set(OBJECT_LAYER_BITS 16)

# Select X86 processor features to use, by default the library compiles with AVX2, if everything is off it will be SSE2 compatible.
set(USE_SSE4_1 ON)
set(USE_SSE4_2 ON)
set(USE_AVX ON)
set(USE_AVX2 ON)
set(USE_AVX512 OFF)
set(USE_LZCNT ON)
set(USE_TZCNT ON)
set(USE_F16C ON)
set(USE_FMADD ON)

ExternalProject_Add(external_jolt
  URL file://${PACKAGE_DIR}/${JOLT_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${JOLT_HASH_TYPE}=${JOLT_HASH}
  PREFIX ${BUILD_DIR}/jolt

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/jolt
    ${DEFAULT_CMAKE_FLAGS}

  INSTALL_DIR ${LIBDIR}/jolt
)
