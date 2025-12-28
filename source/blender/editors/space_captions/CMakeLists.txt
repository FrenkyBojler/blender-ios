# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Include directories for this module
set(INC
  ../include
  ../../makesrna

  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
) 

set(INC_SYS
)

# Source files
set(SRC
  space_captions.cc
  captions_edit.cc
  captions_ops.cc
  captions_intern.hh
)

# Libraries to link against
set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenfont
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::sequencer
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
)

# Create the Blender library
blender_add_lib(bf_editor_space_captions "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# Make sure RNA is built first
add_dependencies(bf_editor_space_captions bf_rna)

# TODO: Clean unused dependencies.
