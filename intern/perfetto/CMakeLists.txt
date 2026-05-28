# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


set(INC
  PUBLIC .
)

set(SRC
  perfetto_trace.cc
  perfetto_trace.hh
)

set(LIB
  PRIVATE bf::extern::perfetto
)

blender_add_lib(intern_perfetto "${SRC}" "${INC}" "" "${LIB}")
add_library(bf::intern::perfetto ALIAS intern_perfetto)
target_compile_definitions(intern_perfetto
    PUBLIC WITH_PERFETTO
)