# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PUBLIC .
)

set(INC_SYS

)

set(SRC
  intern/win32oskey.cc

  # public includes
  win32oskey.hh
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
)

blender_add_lib(bf_intern_win32oskey "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::intern::win32oskey ALIAS bf_intern_win32oskey)
