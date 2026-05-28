# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


set(INC
  PUBLIC .
)

set(SRC
  perfetto.cc
  perfetto.h
)

set(LIB
)

blender_add_lib(extern_perfetto "${SRC}" "${INC}" "" "${LIB}")
add_library(bf::extern::perfetto ALIAS extern_perfetto)
