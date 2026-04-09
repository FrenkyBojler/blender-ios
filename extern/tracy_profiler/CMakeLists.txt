include(ExternalProject)
include(tracy_version.cmake)

set(TRACY_INSTALL_DIR ${CMAKE_BINARY_DIR})

# NOTE: Exceptionally, use tracy_profiler instead of extern_tracy_profiler as target name as this target is meant to
#       be manually built by developers using `make/ninja tracy_profiler`. See ./README.blender for more details.
ExternalProject_Add(tracy_profiler
  GIT_REPOSITORY https://github.com/wolfpld/tracy.git

  GIT_TAG ${TRACY_VERSION}
  PREFIX ${CMAKE_BINARY_DIR}/tracy_profiler
  SOURCE_SUBDIR profiler
  EXCLUDE_FROM_ALL TRUE

  INSTALL_DIR ${TRACY_INSTALL_DIR}

  BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG>

  CMAKE_ARGS
  -DCMAKE_INSTALL_PREFIX:PATH=${TRACY_INSTALL_DIR}
  -DCMAKE_DEBUG_POSTFIX=_d
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
)