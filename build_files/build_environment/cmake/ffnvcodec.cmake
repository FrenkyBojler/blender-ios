# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# NVIDIA codec headers needed to build FFmpeg with NVENC hardware encoding
# support. Header-only: there is nothing to configure or compile, the install
# step copies headers and generates the pkg-config file used by the FFmpeg
# configure. The actual NVENC library is loaded dynamically at runtime.

ExternalProject_Add(external_ffnvcodec
  URL file://${PACKAGE_DIR}/${FFNVCODEC_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${FFNVCODEC_HASH_TYPE}=${FFNVCODEC_HASH}
  PREFIX ${BUILD_DIR}/ffnvcodec

  CONFIGURE_COMMAND echo .

  BUILD_COMMAND echo .

  INSTALL_COMMAND ${CONFIGURE_ENV} &&
    cd ${BUILD_DIR}/ffnvcodec/src/external_ffnvcodec/ &&
    make PREFIX=${LIBDIR}/ffnvcodec install

  INSTALL_DIR ${LIBDIR}/ffnvcodec
)
