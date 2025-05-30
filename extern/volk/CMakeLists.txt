# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
)

set(INC_SYS
  ${VULKAN_INCLUDE_DIRS}
  ${MOLTENVK_INCLUDES}
)

# volk.c is not a translation unit, it is included by volk.h but we 
# still want it to show up in the various IDE projects, so mark the file
# as header only (even though it has a .cc extention) to prevent the 
# build system from trying to build it.
set(VOLK_HEADER_UNITS
  volk.c
)
set_source_files_properties(${VOLK_HEADER_UNITS} PROPERTIES HEADER_FILE_ONLY TRUE)

set(SRC
  volk_impl.cc
  volk.h
  ${VOLK_HEADER_UNITS}
)

set(LIB
)

blender_add_lib(extern_volk "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
