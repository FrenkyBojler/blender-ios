# # SPDX-FileCopyrightText: 2025 Blender Authors
# #
# # SPDX-License-Identifier: GPL-2.0-or-later

ExternalProject_Add(external_rubberband
  URL file://${PACKAGE_DIR}/${RUBBERBAND_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${RUBBERBAND_HASH_TYPE}=${RUBBERBAND_HASH}
  PREFIX ${BUILD_DIR}/rubberband

  CONFIGURE_COMMAND ${CONFIGURE_ENV} &&
    ${MESON} setup
      --prefix ${LIBDIR}/rubberband
      ${MESON_BUILD_TYPE}
      -Dauto_features=disabled
      -Ddefault_library=static
      ${BUILD_DIR}/rubberband/src/external_rubberband-build
      ${BUILD_DIR}/rubberband/src/external_rubberband


  BUILD_COMMAND ninja
  INSTALL_COMMAND ninja install
  INSTALL_DIR ${LIBDIR}/rubberband
)

add_dependencies(
  external_rubberband
  # Needed for `MESON`.
  external_python_site_packages
)

if(WIN32)
  if(BUILD_MODE STREQUAL Release)
    ExternalProject_Add_Step(external_rubberband after_install
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/rubberband/include
        ${HARVEST_TARGET}/rubberband/include
      COMMAND ${CMAKE_COMMAND} -E copy
        ${LIBDIR}/rubberband/lib/rubberband-static.lib
        ${HARVEST_TARGET}/rubberband/lib/rubberband-static.lib
      DEPENDEES install
    )
  else()
    ExternalProject_Add_Step(external_rubberband after_install
      COMMAND ${CMAKE_COMMAND} -E copy
        ${LIBDIR}/rubberband/lib/rubberband-static.lib
        ${HARVEST_TARGET}/rubberband/lib/rubberband-static_d.lib
      DEPENDEES install
    )  
  endif()
else()
  harvest(external_rubberband rubberband/include rubberband/include "*.h")
  harvest(external_rubberband rubberband/lib rubberband/lib "*.a")
endif()
