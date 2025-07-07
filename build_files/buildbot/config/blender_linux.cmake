# SPDX-FileCopyrightText: 2015-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ######## Global feature set settings ########

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/config/blender_release.cmake")

message(STATUS "Building in Rocky 8 Linux 64bit environment")

# ######## Linux-specific build options ########
# Options which are specific to Linux-only platforms

set(WITH_DOC_MANPAGE         OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_TEST_OSL     ON CACHE BOOL "" FORCE)

set(HIPRT_COMPILER_PARALLEL_JOBS        6 CACHE STRING "" FORCE)
set(SYCL_OFFLINE_COMPILER_PARALLEL_JOBS 6 CACHE STRING "" FORCE)

get_property(WITH_COMPILER_ASAN GLOBAL PROPERTY WITH_COMPILER_ASAN)
message(STATUS "Checking for ASAN flag: ${WITH_COMPILER_ASAN}")
if(WITH_COMPILER_ASAN)
  message(STATUS "Building a sanitizer build")

  set(WITH_STRSIZE_DEBUG ON CACHE BOOL "" FORCE)
  
  # Current builders do not have enough RAM/Cores ratio to survive a full-core ASAN build.
  # Trusting automatic tests for max jobs setup for the time being.
  set(WITH_NINJA_POOL_JOBS ON CACHE BOOL "" FORCE)
endif()


set(WITH_LINUX_OFFICIAL_RELEASE_TESTS   ON CACHE BOOL "" FORCE)

# Validate that some python scripts in out `build_files` and `docs` directories
# can be used with the builder's system python.
set(WITH_SYSTEM_PYTHON_TESTS ON CACHE BOOL "" FORCE)
set(TEST_SYSTEM_PYTHON_EXE "/usr/bin/python3.6" CACHE PATH "" FORCE)
