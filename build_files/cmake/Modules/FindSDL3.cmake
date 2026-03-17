# SPDX-FileCopyrightText: 2015 Blender Authors
#
# SPDX-License-Identifier: BSD-3-Clause

# - Find SDL library
# Find the native SDL includes and library
# This module defines
#  SDL3_INCLUDE_DIRS, where to find SDL.h, Set when SDL3_INCLUDE_DIR is found.
#  SDL3_LIBRARIES, libraries to link against to use SDL.
#  SDL3_ROOT_DIR, The base directory to search for SDL.
#                This can also be an environment variable.
#  SDL3_FOUND, If false, do not try to use SDL.
#
# also defined, but not for general use are
#  SDL3_LIBRARY, where to find the SDL library.

# If `SDL3_ROOT_DIR` was defined in the environment, use it.
if(DEFINED SDL3_ROOT_DIR)
  # Pass.
elseif(DEFINED ENV{SDL3_ROOT_DIR})
  set(SDL3_ROOT_DIR $ENV{SDL3_ROOT_DIR})
else()
  set(SDL3_ROOT_DIR "")
endif()

set(_sdl3_SEARCH_DIRS
  ${SDL3_ROOT_DIR}
)

find_path(SDL3_INCLUDE_DIR
  NAMES
  SDL.h
  HINTS
    ${_sdl3_SEARCH_DIRS}
  PATH_SUFFIXES
    include/SDL3 include SDL3
)

find_library(SDL3_LIBRARY
  NAMES
  SDL3
  HINTS
    ${_sdl3_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
)

# handle the QUIETLY and REQUIRED arguments and set SDL3_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL3 DEFAULT_MSG
    SDL3_LIBRARY SDL3_INCLUDE_DIR)

if(SDL3_FOUND)
  set(SDL3_LIBRARIES ${SDL3_LIBRARY})
  set(SDL3_INCLUDE_DIRS ${SDL3_INCLUDE_DIR})
endif()

mark_as_advanced(
  SDL3_INCLUDE_DIR
  SDL3_LIBRARY
)

unset(_sdl3_SEARCH_DIRS)
