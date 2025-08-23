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