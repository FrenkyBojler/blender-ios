# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/config/blender_release.cmake")

# Define CMAKE_OSX_ARCHITECTURES if not set.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/platform/platform_apple_xcode.cmake")

set(WITH_CYCLES_TEST_OSL     ON CACHE BOOL "" FORCE)

if("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "x86_64")
  # The legacy linker emits fewer warnings than the modern one.
  #
  # It is also a lot slower, but in CI the warnings amount is more important
  # than the extra minutes it takes to link.
  set(WITH_LINKER_LEGACY ON CACHE BOOL "" FORCE)
  mark_as_advanced(WITH_LINKER_LEGACY)
endif()
