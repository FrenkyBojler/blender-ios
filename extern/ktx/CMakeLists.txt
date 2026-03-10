# SPDX-FileCopyrightText: 2026 KhronosGroup
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Build ktx library.
add_subdirectory(ktx)

set(INC
  ktx/ktx
)

# Build ktx-Blender bridging module.
set(SRC
  src/common.h
)

set(LIB
  ktx
)

add_library(extern_ktx SHARED "${SRC}")
target_include_directories(extern_ktx PUBLIC "${INC}")
target_link_libraries(extern_ktx PUBLIC "${LIB}")

blender_source_group(extern_ktx "${SRC}")
