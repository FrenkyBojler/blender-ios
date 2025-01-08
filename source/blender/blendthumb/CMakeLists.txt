# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# -----------------------------------------------------------------------------
# Shared Thumbnail Extraction Logic

include_directories(
  ../blenlib
  ../makesdna
  ../../../intern/guardedalloc
)

include_directories(
  SYSTEM
  ${ZLIB_INCLUDE_DIRS}
)

set(SRC
  src/blendthumb.hh
  src/blendthumb_extract.cc
  src/blendthumb_png.cc
)

if(WIN32)
  # -----------------------------------------------------------------------------
  # Build `BlendThumb.dll`

  set(SRC_WIN32
    src/blendthumb_win32.cc
    src/blendthumb_win32.def
    src/blendthumb_win32.rc
    src/blendthumb_win32_dll.cc
  )

  add_definitions(-DNOMINMAX)

  add_library(BlendThumb SHARED ${SRC} ${SRC_WIN32})

  target_link_libraries(BlendThumb bf_blenlib dbghelp.lib Version.lib)
  set_target_properties(BlendThumb PROPERTIES LINK_FLAGS_DEBUG "/NODEFAULTLIB:msvcrt")
  set_target_properties(BlendThumb PROPERTIES VS_GLOBAL_VcpkgEnabled "false")

elseif(APPLE)
  # -----------------------------------------------------------------------------
  # Build `blender-thumbnailer.appex` app extension.
  set(SRC_APPEX
    src/thumbnail_provider.mm
    src/thumbnail_provider.h
  )

  add_executable(blender-thumbnailer MACOSX_BUNDLE ${SRC} ${SRC_APPEX})
  setup_platform_linker_flags(blender-thumbnailer)
  setup_platform_linker_libs(blender-thumbnailer)
  target_link_libraries(blender-thumbnailer
    bf_blenlib
    # Avoid linker error about undefined _main symbol.
    "-e _NSExtensionMain"
    "-framework QuickLookThumbnailing"
  )
elseif(UNIX)
  # -----------------------------------------------------------------------------
  # Build `blender-thumbnailer` executable

  set(SRC_CMD
    src/blender_thumbnailer.cc
  )

  add_executable(blender-thumbnailer ${SRC} ${SRC_CMD})
  setup_platform_linker_flags(blender-thumbnailer)
  target_link_libraries(blender-thumbnailer bf_blenlib)
  if(DEFINED PTHREADS_LIBRARIES)
    target_link_libraries(blender-thumbnailer ${PTHREADS_LIBRARIES})
  endif()
endif()
