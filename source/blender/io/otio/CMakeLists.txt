# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  exporter
  importer
  ../../editors/include
  ../../makesrna
  $ENV{HOME}/opentimelineio/include
)

set(INC_SYS
)

set(SRC
  IO_otio.cc
  exporter/otio_export_strip.cc
  exporter/otio_export.cc
  importer/otio_import.cc

  IO_otio.hh
  exporter/otio_export_strip.hh
  exporter/otio_export.hh
  importer/otio_import.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blentranslation
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::imbuf
  PRIVATE bf::sequencer
  PRIVATE bf::windowmanager
  PRIVATE "$ENV{HOME}/opentimelineio/lib/libopentimelineio.a"
)


blender_add_lib(bf_io_otio "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

