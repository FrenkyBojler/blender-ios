# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(TEXTCONV_INC
)

set(TEXTCONV_SRC
  intern/blend_textconv.cc
)

set(TEXTCONV_LIB
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader_core
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::extern::fmtlib
)

set(BLEND_DIFF_INC
)

set(BLEND_DIFF_SRC
  intern/blend_diff.cc
)

set(BLEND_DIFF_LIB
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader_core
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::extern::fmtlib
  PRIVATE bf::extern::xxhash
)

add_executable(blend_diff ${BLEND_DIFF_SRC})
setup_platform_linker_flags(blend_diff)
blender_target_include_dirs(blend_diff ${BLEND_DIFF_INC})
target_link_libraries(blend_diff ${BLEND_DIFF_LIB})
