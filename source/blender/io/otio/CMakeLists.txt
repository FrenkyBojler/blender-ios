# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  exporter
  ../common
  ../../blenkernel
  ../../blenlib
  ../../makesdna
  ../../makesrna
  ../../sequencer
  ../../editors/include
)

set(INC_SYS
  ${OPENTIMELINEIO_INCLUDE_DIRS}
  ${OPENTIME_INCLUDE_DIRS}
)

set(SRC
  IO_otio.cc
  exporter/otio_exporter.cc

  IO_otio.hh
  exporter/otio_exporter.hh
)

set(LIB
  bf_io_common
  bf::blenkernel
  bf::blenlib
  bf::intern::guardedalloc
  bf::windowmanager
  ${OPENTIMELINEIO_LIBRARIES}
  ${OPENTIME_LIBRARIES}
)

blender_add_lib(bf_io_otio "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
