# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
)

set(INC_SYS
)

set(SRC
  xatlas.cpp
  xatlas.h
)

set(LIB
)

blender_add_lib(extern_xatlas "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::extern::xatlas ALIAS extern_xatlas)
