# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


set(SENTRY_SDK_EXTRA_ARGS

)

ExternalProject_Add(external_sentry_sdk
  URL file://${PACKAGE_DIR}/${SENTRY_SDK_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${SENTRY_SDK_HASH_TYPE}=${SENTRY_SDK_HASH}
  PREFIX ${BUILD_DIR}/sentry_sdk
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}
  
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/sentry_sdk
    ${DEFAULT_CMAKE_FLAGS}
    ${SENTRY_SDK_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/sentry_sdk
)

if(WIN32)
  if(BUILD_MODE STREQUAL Release)
    ExternalProject_Add_Step(external_sentry_sdk after_install
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${LIBDIR}/sentry_sdk
        ${HARVEST_TARGET}/sentry_sdk

      DEPENDEES install
    )
  endif()
else()
  #TODO
  #harvest(external_sentry_sdk .....)
endif()