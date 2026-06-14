# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
)

set(INC_SYS
)

set(SRC
  intern/midi.cc

  IO_midi.hh
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
)

blender_add_lib(bf_io_midi "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

if(WITH_GTESTS)
  set(TEST_SRC
    tests/midi_test.cc
  )
  set(TEST_INC
    ../../../../tests/gtests
  )
  set(TEST_LIB
    bf_io_midi
  )
  blender_add_test_suite_lib(io_midi "${TEST_SRC}" "${INC};${TEST_INC}" "${INC_SYS}" "${LIB};${TEST_LIB}")
endif()
