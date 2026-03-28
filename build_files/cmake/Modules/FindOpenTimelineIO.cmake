# SPDX-FileCopyrightText: 2015 Blender Authors
#
# SPDX-License-Identifier: BSD-3-Clause

# - Find OpenTimelineIO library
# also OpenTime library






if(DEFINED OPENTIMELINEIO_ROOT_DIR)
  # Pass
elseif(DEFINED ENV{OPENTIMELINEIO_ROOT_DIR})
  set(OPENTIMELINEIO_ROOT_DIR $ENV{OPENTIMELINEIO_ROOT_DIR})
else()
  set(OPENTIMELINEIO_ROOT_DIR "")
endif()

set(opentimelineio_SEARCH_DIRS
  ${OPENTIMELINEIO_ROOT_DIR}
)

find_path(OPENTIMELINEIO_INCLUDE_DIR
  NAMES
    # The NAMES here include the "opentimelineio/" subdirectory prefix,
    # so PATH_SUFFIXES must be "include", not "include/opentimelineio".
    # Example resolved path: /usr/local/include/opentimelineio/version.h
    opentimelineio/version.h
    opentimelineio/clip.h
    opentimelineio/anyDictionary.h
    opentimelineio/anyVector.h
    opentimelineio/color.h
    opentimelineio/composable.h
    opentimelineio/composition.h
    opentimelineio/deserialization.h
    opentimelineio/editAlgorithm.h
    opentimelineio/effect.h
    opentimelineio/errorStatus.h
    opentimelineio/export.h
    opentimelineio/externalReference.h
    opentimelineio/freezeFrame.h
    opentimelineio/gap.h
    opentimelineio/generatorReference.h
    opentimelineio/imageSequenceReference.h
    opentimelineio/item.h
    opentimelineio/linearTimeWarp.h
    opentimelineio/marker.h
    opentimelineio/mediaReference.h
    opentimelineio/missingReference.h
    opentimelineio/safely_typed_any.h
    opentimelineio/serializableCollection.h
    opentimelineio/serializableObject.h
    opentimelineio/serializableObjectWithMetadata.h
    opentimelineio/serialization.h
    opentimelineio/stackAlgorithm.h
    opentimelineio/stack.h
    opentimelineio/timeEffect.h
    opentimelineio/timeline.h
    opentimelineio/trackAlgorithm.h
    opentimelineio/track.h
    opentimelineio/transition.h
    opentimelineio/typeRegistry.h
    opentimelineio/unknownSchema.h
    opentimelineio/vectorIndexing.h
  HINTS
    ${OPENTIMELINEIO_ROOT_DIR}
  PATH_SUFFIXES
    include
)

find_library(OPENTIMELINEIO_LIBRARY
  NAMES
    opentimelineio
  HINTS
    ${opentimelineio_SEARCH_DIRS}
  PATH_SUFFIXES
    lib lib64
)

find_path(OPENTIME_INCLUDE_DIR
  NAMES
    # Same reasoning as above: prefix includes "opentime/" so suffix is just "include".
    opentime/errorStatus.h
    opentime/export.h
    opentime/rationalTime.h
    opentime/stringPrintf.h
    opentime/timeRange.h
    opentime/timeTransform.h
    opentime/version.h
  HINTS
    ${OPENTIMELINEIO_ROOT_DIR}
  PATH_SUFFIXES
    include
)

find_library(OPENTIME_LIBRARY
  NAMES
    opentime
  HINTS
    ${opentimelineio_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenTimelineIO DEFAULT_MSG
    OPENTIMELINEIO_LIBRARY OPENTIMELINEIO_INCLUDE_DIR)
find_package_handle_standard_args(OpenTime DEFAULT_MSG
    OPENTIME_LIBRARY OPENTIME_INCLUDE_DIR)

if(OPENTIMELINEIO_FOUND)
  set(OPENTIMELINEIO_LIBRARIES ${OPENTIMELINEIO_LIBRARY})
  set(OPENTIMELINEIO_INCLUDE_DIRS ${OPENTIMELINEIO_INCLUDE_DIR})
endif()

if(OPENTIME_FOUND)
  set(OPENTIME_LIBRARIES ${OPENTIME_LIBRARY})
  set(OPENTIME_INCLUDE_DIRS ${OPENTIME_INCLUDE_DIR})
endif()

mark_as_advanced(
  OPENTIMELINEIO_INCLUDE_DIR
  OPENTIMELINEIO_INCLUDE_DIRS
  OPENTIMELINEIO_LIBRARY
  OPENTIMELINEIO_LIBRARIES
  OPENTIME_INCLUDE_DIR
  OPENTIME_INCLUDE_DIRS
  OPENTIME_LIBRARY
  OPENTIME_LIBRARIES
)

unset(
    opentimelineio_SEARCH_DIRS
)