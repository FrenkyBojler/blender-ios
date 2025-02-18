# SPDX-FileCopyrightText: 2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# shorthand to only unpack a certain dependency
macro(unpack_only name)
  string(TOUPPER ${name} UPPER_NAME)
  set(TARGET_FILE ${${UPPER_NAME}_FILE})
  set(TARGET_HASH_TYPE ${${UPPER_NAME}_HASH_TYPE})
  set(TARGET_HASH ${${UPPER_NAME}_HASH})
  ExternalProject_Add(external_${name}
    URL file://${PACKAGE_DIR}/${TARGET_FILE}
    URL_HASH ${TARGET_HASH_TYPE}=${TARGET_HASH}
    DOWNLOAD_DIR ${DOWNLOAD_DIR}
    PREFIX ${BUILD_DIR}/${name}
    CONFIGURE_COMMAND echo .
    BUILD_COMMAND echo .
    INSTALL_COMMAND echo .
  )
endmacro()

# turn a c:\something\somefile.txt path into /c/something/somefile.txt 
macro(cmake_to_msys_path MsysPath ResultingPath)
  string(REPLACE ":" "" TmpPath "${MsysPath}")
  string(SUBSTRING ${TmpPath} 0 1 Drive)
  string(SUBSTRING ${TmpPath} 1 255 PathPart)
  string(TOLOWER ${Drive} LowerDrive)
  string(CONCAT ${ResultingPath} "/" ${LowerDrive} ${PathPart})
endmacro()