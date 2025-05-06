# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PUBLIC .
)


set(SRC
  intern/blo_bhead.cc
  intern/blo_blend_header.cc

  BLO_bhead.hh
  BLO_blend_defs.hh
  BLO_blend_header.hh
)

set(LIB
  PRIVATE bf::blenlib
)

if(WIN32)
  add_definitions(-DNOMINMAX)
endif()

blender_add_lib(bf_blenloader_core "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::blenloader_core ALIAS bf_blenloader_core)
