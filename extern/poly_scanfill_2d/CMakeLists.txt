# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PUBLIC .
)

set(INC_SYS
)

set(SRC
  poly_scanfill_2d.cc

  poly_scanfill_2d.hh
)

set(LIB
)

blender_add_lib(extern_poly_scanfill_2d "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::extern::poly_scanfill_2d ALIAS extern_poly_scanfill_2d)

target_compile_features(extern_poly_scanfill_2d PRIVATE cxx_std_20)
