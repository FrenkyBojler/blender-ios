# SPDX-FileCopyrightText: 2019 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Build Draco library.
add_subdirectory(draco)

set(INC
  draco/src
)

# Build Draco-Blender bridging module.
set(SRC
  src/common.cpp
  src/decoder.cpp
  src/encoder.cpp

  src/common.h
  src/decoder.h
  src/encoder.h
)

set(LIB
  draco
)

add_library(extern_draco SHARED "${SRC}")
target_include_directories(extern_draco PUBLIC "${INC}")
target_link_libraries(extern_draco PUBLIC "${LIB}")

blender_source_group(extern_draco "${SRC}")

if (APPLE)
  # On macOS, do not include the built library in Xcode bundle distribution archives (implicit behavior for SHARED libs)
  set_target_properties(extern_draco PROPERTIES
    XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
    XCODE_ATTRIBUTE_INSTALL_PATH ""
  )
endif()
