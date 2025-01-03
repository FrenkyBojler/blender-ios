# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  importer
  ../common
  ../../editors/include
  ../../makesrna
)

set(INC_SYS
)

set(SRC
  IO_fbx.cc
  importer/fbx_import.cc
  importer/fbx_import_material.cc

  IO_fbx.hh
  importer/fbx_import.hh
  importer/fbx_import_material.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::bmesh
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::nodes
  PRIVATE bf::imbuf
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  bf_io_common
  PRIVATE bf::extern::fmtlib
  PRIVATE bf::extern::ufbx
)

blender_add_lib(bf_io_fbx "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
