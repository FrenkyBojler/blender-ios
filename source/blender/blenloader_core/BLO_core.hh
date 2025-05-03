/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <variant>

#include "BLI_endian_switch.h"
#include "BLI_sys_types.h"

struct FileReader;

typedef struct BHead {
  /** Identifier for this #BHead. Can be any of BLO_CODE_* or an ID code like ID_OB.  */
  int code;
  /** Identifier of the struct type that is stored in this block. */
  int SDNAnr;
  /**
   * Identifier the block had when it was written. This is used to remap memory blocks on load.
   * Typically, this is the pointer that the memory had when it was written.
   * This should be unique across the whole blend-file, except for `BLEND_DATA` blocks, which
   * should be unique within a same ID.
   */
  const void *old;
  /** Number of bytes in the block. */
  int64_t len;
  /** Number of structs in the array (1 for simple structs). */
  int64_t nr;
} BHead;

typedef struct BHead4 {
  int code, len;
  uint old;
  int SDNAnr, nr;
} BHead4;

typedef struct SmallBHead8 {
  int code, len;
  uint64_t old;
  int SDNAnr, nr;
} SmallBHead8;

typedef struct LargeBHead8 {
  int code;
  int SDNAnr;
  uint64_t old;
  int64_t len;
  int64_t nr;
} LargeBHead8;

enum class BHeadType {
  BHead4,
  SmallBHead8,
  LargeBHead8,
};

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

BlenderHeaderVariant BLO_readfile_blender_header_decode(FileReader *file);

/** Returns #std::nullopt if the file is exhausted. */
std::optional<BHead> BLO_readfile_read_bhead(FileReader *file,
                                             BHeadType type,
                                             bool do_endian_swap);

/**
 * Converts a BHead.old pointer from 64 to 32 bit. This can't work in the general case, but only
 * when the lower 32 bits of all relevant 64 bit pointers are different. Otherwise two different
 * pointers will map to the same, which will break things later on. There is no way to check for
 * that here unfortunately.
 */
inline uint32_t uint32_from_uint64_ptr(uint64_t ptr, const bool use_endian_swap)
{
  if (use_endian_swap) {
    /* Do endian switch so that the resulting pointer is not all 0. */
    BLI_endian_switch_uint64(&ptr);
  }
  /* Behavior has to match #cast_pointer_64_to_32. */
  ptr >>= 3;
  return uint32_t(ptr);
}
