# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Minimal FontForge build for Blender's overlap removal.
# Only compiles splineoverlap.c and its direct dependencies.

set(INC
  fontforge
)

set(INC_SYS
)

set(SRC
  fontforge/autohint.c
  # fontforge/splineorder2.c
  fontforge/splineoverlap.c
  fontforge/splineoverlap_blender.c
  fontforge/splinerefigure.c
  fontforge/splineutil.c
  fontforge/splineutil2.c
  fontforge/splinefill.c
  fontforge/stubs.c
  fontforge/mem.c

  fontforge/fontforge-config.h
  fontforge/autohint.h
  fontforge/ustring.h
  # fontforge/splineorder2.h
  fontforge/splineoverlap.h
  fontforge/splineoverlap_blender.h
  fontforge/splinefont.h
  fontforge/splineutil2.h
  fontforge/splinefill.h
  fontforge/edgelist2.h
)

set(LIB
)

# Define BLENDER to use our minimal config
add_definitions(-DBLENDER)

blender_add_lib(extern_fontforge "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
