/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BLO_bhead.hh"

/** A header that has been parsed successfully. */
struct BlenderHeader {
  /** 4 or 8. */
  int pointer_size;
  /** L_ENDIAN or B_ENDIAN. */
  int endian;
  /** #BLENDER_FILE_VERSION. */
  int file_version;
  /** #BLEND_FILE_FORMAT_VERSION. */
  int file_format_version;

  BHeadType bhead_type() const;
};

/** The file is detected to be a Blender file, but it could not be decoded successfully. */
struct BlenderHeaderUnknown {};

/** The file is not a Blender file. */
struct BlenderHeaderInvalid {};

using BlenderHeaderVariant =
    std::variant<BlenderHeaderInvalid, BlenderHeaderUnknown, BlenderHeader>;

/**
 * Reads the header at the beginning of a .blend file and decodes it.
 */
BlenderHeaderVariant BLO_readfile_blender_header_decode(FileReader *file);
