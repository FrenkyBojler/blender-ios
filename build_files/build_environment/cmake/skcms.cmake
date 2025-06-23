# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# This is a build time requirement for libjxl. We only have to unpack it
# and libjxl will build it.

ExternalProject_Add(external_skcms
  URL file://${PACKAGE_DIR}/${SKCMS_FILE}
  URL_HASH ${SKCMS_HASH_TYPE}=${SKCMS_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/skcms
  CONFIGURE_COMMAND echo .
  BUILD_COMMAND echo .
  INSTALL_COMMAND echo .
)
