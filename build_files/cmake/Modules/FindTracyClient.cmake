# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: BSD-3-Clause

# - Find TracyClient library
# Find the native TracyClient includes and library
# This module defines
#  TracyClient_INCLUDE_DIRS, where to find <tracy/tracy/Tracy.hpp>, Set when
#                            TracyClient is found.
#  TracyClient_LIBRARIES, libraries to link against to use TracyClient.
#  TracyClient_ROOT_DIR, The base directory to search for TracyClient.
#                        This can also be an environment variable.
#  TracyClient_FOUND, If false, do not try to use TracyClient.
#
# also defined, but not for general use are
#  TracyClient_LIBRARY, where to find the TracyClient library.

# If `TracyClient_ROOT_DIR` was defined in the environment, use it.
if(DEFINED TracyClient_ROOT_DIR)
  # Pass.
elseif(DEFINED ENV{TracyClient_ROOT_DIR})
  set(TracyClient_ROOT_DIR $ENV{TracyClient_ROOT_DIR})
else()
  set(TracyClient_ROOT_DIR "")
endif()

set(_tracy_client_SEARCH_DIRS
  ${TracyClient_ROOT_DIR}
  /opt/lib/tracy
)

find_path(TracyClient_INCLUDE_DIR
  NAMES
  tracy/tracy/Tracy.hpp
  HINTS
  ${_tracy_client_SEARCH_DIRS}
  PATH_SUFFIXES
  include
)

find_library(TracyClient_LIBRARY
  NAMES
  TracyClient
  HINTS
  ${_tracy_client_SEARCH_DIRS}
  PATH_SUFFIXES
  lib64 lib
)

# handle the QUIETLY and REQUIRED arguments and set TracyClient_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TracyClient DEFAULT_MSG
  TracyClient_LIBRARY TracyClient_INCLUDE_DIR)

if(TracyClient_FOUND)
  set(TracyClient_LIBRARIES ${TracyClient_LIBRARY})
  set(TracyClient_INCLUDE_DIRS ${TracyClient_INCLUDE_DIR})
else()
  set(TracyClient_TracyClient_FOUND FALSE)
endif()

mark_as_advanced(
  TracyClient_INCLUDE_DIR
  TracyClient_LIBRARY
)

unset(_tracy_client_SEARCH_DIRS)