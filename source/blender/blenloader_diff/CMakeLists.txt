# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
)

set(SRC
  intern/blend_diff.cc
  intern/blend_query.cc

  intern/blend_query.hh
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader_core
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::extern::fmtlib
  PRIVATE bf::extern::xxhash
)

add_executable(blend_diff ${SRC})
setup_platform_linker_flags(blend_diff)
setup_platform_linker_libs(blend_diff)
blender_target_include_dirs(blend_diff ${INC})
target_link_libraries(blend_diff ${LIB})

install(
  TARGETS blend_diff
  DESTINATION "."
)
install(
  PROGRAMS intern/blend_diff_git_driver.py
  DESTINATION "."
)
