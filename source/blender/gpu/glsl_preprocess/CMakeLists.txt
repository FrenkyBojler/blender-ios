# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


# -----------------------------------------------------------------------------
# Build `glsl_preprocess` executable.
set(SRC
  glsl_preprocess.cc

  glsl_preprocess.hh
)

if(WITH_GPU_SHADER_ASSERT)
  add_definitions(-DWITH_GPU_SHADER_ASSERT)
endif()

# `SRC_DNA_INC` is defined in the parent directory.
add_executable(glsl_preprocess ${SRC})
# Force compiler optimization to avoid very bad processing time on MSVC.
# Can be disabled for debugging and making change to the executable.
target_compile_options(glsl_preprocess PRIVATE -O3)
