# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
)

set(INC_SYS
)

set(SRC
  intern/blend_textconv.cc
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader_core
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::extern::fmtlib
  PRIVATE bf::extern::xxhash
)

add_executable(blend_textconv ${SRC})
setup_platform_linker_flags(blend_textconv)
blender_target_include_dirs(blend_textconv ${INC})
target_link_libraries(blend_textconv ${LIB})
