# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: BSD-3-Clause

# - Find Jolt library
# Find the native Jolt Physics includes and library
# This module defines
#  JOLT_INCLUDE_DIRS, where to find Jolt.h, set when
#                     JOLT_INCLUDE_DIR is found.
#  JOLT_LIBRARIES, libraries to link against to use Jolt.
#  JOLT_ROOT_DIR, The base directory to search for Jolt.
#                 This can also be an environment variable.
#  JOLT_FOUND, If false, do not try to use Jolt.
#
# also defined, but not for general use are
#  JOLT_LIBRARY, where to find the Jolt library.

# If `JOLT_ROOT_DIR` was defined in the environment, use it.
if(DEFINED JOLT_ROOT_DIR)
  # Pass.
elseif(DEFINED ENV{JOLT_ROOT_DIR})
  set(JOLT_ROOT_DIR $ENV{JOLT_ROOT_DIR})
else()
  set(JOLT_ROOT_DIR "")
endif()

set(_jolt_SEARCH_DIRS
  ${JOLT_ROOT_DIR}
  /opt/lib/Jolt
)

find_path(JOLT_INCLUDE_DIR
  NAMES
    Jolt/Jolt.h
  HINTS
    ${_jolt_SEARCH_DIRS}
  PATH_SUFFIXES
    include
)

find_library(JOLT_LIBRARY
  NAMES
    Jolt
  HINTS
    ${_jolt_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
)

# Handle the QUIETLY and REQUIRED arguments and set JOLT_FOUND to TRUE if
# all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Jolt DEFAULT_MSG JOLT_LIBRARY JOLT_INCLUDE_DIR)

if(JOLT_FOUND)
  set(JOLT_LIBRARIES ${JOLT_LIBRARY})
  set(JOLT_INCLUDE_DIRS ${JOLT_INCLUDE_DIR})
endif()

mark_as_advanced(
  JOLT_INCLUDE_DIR
  JOLT_LIBRARY
)

unset(_jolt_SEARCH_DIRS)
