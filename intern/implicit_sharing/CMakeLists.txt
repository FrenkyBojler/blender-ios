# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(bf_intern_implicit_sharing INTERFACE)
target_include_directories(bf_intern_implicit_sharing INTERFACE .)
add_library(bf::intern::implicit_sharing ALIAS bf_intern_implicit_sharing)

# CMake 3.19+ allows one to populate the interface library with
# source files to show in the IDE.
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.19")
  set(SRC
    implicit_sharing.hh
    implicit_sharing_ptr.hh
  )
  target_sources(bf_intern_implicit_sharing PRIVATE ${SRC})
  blender_source_group(bf_intern_implicit_sharing ${SRC})
endif()
