# SPDX-FileCopyrightText: 2012-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

if(WIN32)
  set(SDL_EXTRA_ARGS
    -DSDL_STATIC=Off
  )
else()
  set(SDL_EXTRA_ARGS
    -DSDL_STATIC=ON
    -DSDL_SHARED=OFF
    -DSDL_VIDEO=OFF
    -DSDL_SNDIO=OFF
  )
endif()

ExternalProject_Add(external_sdl
  URL file://${PACKAGE_DIR}/${SDL_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${SDL_HASH_TYPE}=${SDL_HASH}
  PREFIX ${BUILD_DIR}/sdl

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/sdl
    ${DEFAULT_CMAKE_FLAGS}
    ${SDL_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/sdl
)

if(WIN32)
  if(BUILD_MODE STREQUAL Release)
    ExternalProject_Add_Step(external_sdl after_install
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/sdl/include/
        ${HARVEST_TARGET}/sdl/include
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/sdl/lib
        ${HARVEST_TARGET}/sdl/lib
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/sdl/bin
        ${HARVEST_TARGET}/sdl/lib

      DEPENDEES install
    )
  endif()
else()
  harvest(external_sdl sdl/include sdl/include "*.h")
  # Harvest both SDL3.a and SDL3-test.a to satisfy the SDL CMake config files used by find_package.
  harvest(external_sdl sdl/lib sdl/lib "*.a")
  harvest(external_sdl sdl/lib/cmake/SDL3 sdl/lib/cmake/SDL3 "*.cmake")
endif()
