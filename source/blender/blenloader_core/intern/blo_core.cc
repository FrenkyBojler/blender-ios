/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "BLO_bhead.hh"
#include "BLO_blend_defs.hh"
#include "BLO_blend_header.hh"

#include "BLI_assert.h"
#include "BLI_endian_defines.h"
#include "BLI_endian_switch.h"
#include "BLI_filereader.h"

BHeadType BlenderHeader::bhead_type() const
{
  if (this->pointer_size == 4) {
    return BHeadType::BHead4;
  }
  if (this->file_format_version == BLEND_FILE_FORMAT_VERSION_0) {
    return BHeadType::SmallBHead8;
  }
  BLI_assert(this->file_format_version == BLEND_FILE_FORMAT_VERSION_1);
  return BHeadType::LargeBHead8;
}

BlenderHeaderVariant BLO_readfile_blender_header_decode(FileReader *file)
{
  char header_bytes[MAX_SIZEOFBLENDERHEADER];
  /* We read the minimal number of header bytes first. If necessary, the remaining bytes are read
   * below. */
  int64_t readsize = file->read(file, header_bytes, MIN_SIZEOFBLENDERHEADER);
  if (readsize != MIN_SIZEOFBLENDERHEADER) {
    return BlenderHeaderInvalid{};
  }
  if (!STREQLEN(header_bytes, "BLENDER", 7)) {
    return BlenderHeaderInvalid{};
  }
  /* If the first 7 bytes are BLENDER, it is very likely that this is a newer version of the
   * blendfile format. If the rest of the decode fails, we can still report that this was a Blender
   * file of a potentially future version. */

  BlenderHeader header;
  /* In the old header format, the next bytes indicate the pointer size. In the new format a
   * version number comes next. */
  const bool is_legacy_header = ELEM(header_bytes[7], '_', '-');

  if (is_legacy_header) {
    header.file_format_version = 0;
    switch (header_bytes[7]) {
      case '_':
        header.pointer_size = 4;
        break;
      case '-':
        header.pointer_size = 8;
        break;
      default:
        return BlenderHeaderUnknown{};
    }
    switch (header_bytes[8]) {
      case 'v':
        header.endian = L_ENDIAN;
        break;
      case 'V':
        header.endian = B_ENDIAN;
        break;
      default:
        return BlenderHeaderUnknown{};
    }
    if (!isdigit(header_bytes[9]) || !isdigit(header_bytes[10]) || !isdigit(header_bytes[11])) {
      return BlenderHeaderUnknown{};
    }
    char version_str[4];
    memcpy(version_str, header_bytes + 9, 3);
    version_str[3] = '\0';
    header.file_version = atoi(version_str);
    return header;
  }

  if (!isdigit(header_bytes[7]) || !isdigit(header_bytes[8])) {
    return BlenderHeaderUnknown{};
  }
  char header_size_str[3];
  memcpy(header_size_str, header_bytes + 7, 2);
  header_size_str[2] = '\0';
  const int header_size = atoi(header_size_str);
  if (header_size != MAX_SIZEOFBLENDERHEADER) {
    return BlenderHeaderUnknown{};
  }

  /* Read remaining header bytes. */
  const int64_t remaining_bytes_to_read = header_size - MIN_SIZEOFBLENDERHEADER;
  readsize = file->read(file, header_bytes + MIN_SIZEOFBLENDERHEADER, remaining_bytes_to_read);
  if (readsize != remaining_bytes_to_read) {
    return BlenderHeaderUnknown{};
  }
  if (header_bytes[9] != '-') {
    return BlenderHeaderUnknown{};
  }
  header.pointer_size = 8;
  if (!isdigit(header_bytes[10]) || !isdigit(header_bytes[11])) {
    return BlenderHeaderUnknown{};
  }
  char blend_file_version_format_str[3];
  memcpy(blend_file_version_format_str, header_bytes + 10, 2);
  blend_file_version_format_str[2] = '\0';
  header.file_format_version = atoi(blend_file_version_format_str);
  if (header.file_format_version != 1) {
    return BlenderHeaderUnknown{};
  }
  if (header_bytes[12] != 'v') {
    return BlenderHeaderUnknown{};
  }
  header.endian = L_ENDIAN;
  if (!isdigit(header_bytes[13]) || !isdigit(header_bytes[14]) || !isdigit(header_bytes[15]) ||
      !isdigit(header_bytes[16]))
  {
    return BlenderHeaderUnknown{};
  }
  char version_str[5];
  memcpy(version_str, header_bytes + 13, 4);
  version_str[4] = '\0';
  header.file_version = std::atoi(version_str);
  return header;
}

static void switch_endian_bh4(BHead4 *bhead)
{
  /* the ID_.. codes */
  if ((bhead->code & 0xFFFF) == 0) {
    bhead->code >>= 16;
  }

  if (bhead->code != BLO_CODE_ENDB) {
    BLI_endian_switch_int32(&bhead->len);
    BLI_endian_switch_int32(&bhead->SDNAnr);
    BLI_endian_switch_int32(&bhead->nr);
  }
}

static void switch_endian_small_bh8(SmallBHead8 *bhead)
{
  /* The ID_* codes. */
  if ((bhead->code & 0xFFFF) == 0) {
    bhead->code >>= 16;
  }

  if (bhead->code != BLO_CODE_ENDB) {
    BLI_endian_switch_int32(&bhead->len);
    BLI_endian_switch_int32(&bhead->SDNAnr);
    BLI_endian_switch_int32(&bhead->nr);
  }
}

static void switch_endian_large_bh8(LargeBHead8 *bhead)
{
  /* The ID_* codes. */
  if ((bhead->code & 0xFFFF) == 0) {
    bhead->code >>= 16;
  }

  if (bhead->code != BLO_CODE_ENDB) {
    BLI_endian_switch_int64(&bhead->len);
    BLI_endian_switch_int32(&bhead->SDNAnr);
    BLI_endian_switch_int64(&bhead->nr);
  }
}

static BHead bhead_from_bhead4(const BHead4 &bhead4)
{
  BHead bhead;
  bhead.code = bhead4.code;
  bhead.len = bhead4.len;
  bhead.old = reinterpret_cast<const void *>(uintptr_t(bhead4.old));
  bhead.SDNAnr = bhead4.SDNAnr;
  bhead.nr = bhead4.nr;
  return bhead;
}

static const void *old_ptr_from_uint64_ptr(const uint64_t ptr, const bool use_endian_swap)
{
  if constexpr (sizeof(void *) == 8) {
    return reinterpret_cast<const void *>(ptr);
  }
  else {
    return reinterpret_cast<const void *>(uintptr_t(uint32_from_uint64_ptr(ptr, use_endian_swap)));
  }
}

static BHead bhead_from_small_bhead8(const SmallBHead8 &small_bhead8, const bool use_endian_swap)
{
  BHead bhead;
  bhead.code = small_bhead8.code;
  bhead.len = small_bhead8.len;
  bhead.old = old_ptr_from_uint64_ptr(small_bhead8.old, use_endian_swap);
  bhead.SDNAnr = small_bhead8.SDNAnr;
  bhead.nr = small_bhead8.nr;
  return bhead;
}

static BHead bhead_from_large_bhead8(const LargeBHead8 &large_bhead8, const bool use_endian_swap)
{
  BHead bhead;
  bhead.code = large_bhead8.code;
  bhead.len = large_bhead8.len;
  bhead.old = old_ptr_from_uint64_ptr(large_bhead8.old, use_endian_swap);
  bhead.SDNAnr = large_bhead8.SDNAnr;
  bhead.nr = large_bhead8.nr;
  return bhead;
}

std::optional<BHead> BLO_readfile_read_bhead(FileReader *file,
                                             const BHeadType type,
                                             const bool do_endian_swap)
{
  switch (type) {
    case BHeadType::BHead4: {
      BHead4 bhead4{};
      bhead4.code = BLO_CODE_DATA;
      const int64_t readsize = file->read(file, &bhead4, sizeof(bhead4));
      if (readsize == sizeof(bhead4) || bhead4.code == BLO_CODE_ENDB) {
        if (do_endian_swap) {
          switch_endian_bh4(&bhead4);
        }
        return bhead_from_bhead4(bhead4);
      }
      break;
    }
    case BHeadType::SmallBHead8: {
      SmallBHead8 small_bhead8{};
      small_bhead8.code = BLO_CODE_DATA;
      const int64_t readsize = file->read(file, &small_bhead8, sizeof(small_bhead8));
      if (readsize == sizeof(small_bhead8) || small_bhead8.code == BLO_CODE_ENDB) {
        if (do_endian_swap) {
          switch_endian_small_bh8(&small_bhead8);
        }
        return bhead_from_small_bhead8(small_bhead8, do_endian_swap);
      }
      break;
    }
    case BHeadType::LargeBHead8: {
      LargeBHead8 large_bhead8{};
      large_bhead8.code = BLO_CODE_DATA;
      const int64_t readsize = file->read(file, &large_bhead8, sizeof(large_bhead8));
      if (readsize == sizeof(large_bhead8) || large_bhead8.code == BLO_CODE_ENDB) {
        if (do_endian_swap) {
          switch_endian_large_bh8(&large_bhead8);
        }
        return bhead_from_large_bhead8(large_bhead8, do_endian_swap);
      }
      break;
    }
  }
  return std::nullopt;
}
