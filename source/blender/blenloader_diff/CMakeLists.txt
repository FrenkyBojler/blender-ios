# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PRIVATE ../../../extern/toml11
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
  PRIVATE bf::extern::xxhash
)

add_executable(blend_diff ${SRC})
setup_platform_linker_flags(blend_diff)
setup_platform_linker_libs(blend_diff)
blender_target_include_dirs(blend_diff ${INC})
target_link_libraries(blend_diff ${LIB})

if(APPLE)
  set(INSTALL_DEST "$<TARGET_BUNDLE_CONTENT_DIR:blender>/MacOS")
else()
  set(INSTALL_DEST ".")
endif()

install(
  TARGETS blend_diff
  RUNTIME DESTINATION "${INSTALL_DEST}"
)
install(
  PROGRAMS intern/blend_diff_git_driver.py
  DESTINATION "${INSTALL_DEST}"
)
install(
  FILES intern/blend_diff_config.toml
  DESTINATION "${INSTALL_DEST}"
)