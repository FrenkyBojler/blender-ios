# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
set(INC
  .
)
set(INC_SYS
)
set(SRC
  pch.cc
)
set(LIB
)
blender_add_lib(bf_pch "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
if(COMMAND target_precompile_headers)
  target_precompile_headers(bf_pch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pch.hh")
endif()
