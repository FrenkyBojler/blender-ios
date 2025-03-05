# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/config/blender_release.cmake")

set(WITH_CYCLES_TEST_OSL     ON CACHE BOOL "" FORCE)

if(${XCODE_VERSION} VERSION_GREATER_EQUAL 15.0 AND "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "x86_64")
  # Ref: https://projects.blender.org/blender/blender/pulls/134639#issuecomment-1513274
  set(WITH_FORCE_OLD_LINKER ON CACHE BOOL "" FORCE)
  mark_as_advanced(WITH_FORCE_OLD_LINKER)
endif()
