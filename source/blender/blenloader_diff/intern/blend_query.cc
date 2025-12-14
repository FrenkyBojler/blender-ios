/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "blend_query.hh"

#include "BLI_string.h"
#include "BLI_timeit.hh"

#include "BLO_core_blend_header.hh"
#include "BLO_core_file_reader.hh"

namespace blender::blend_query {

using rich_sdna::PrimitiveType;
using rich_sdna::PrimitiveValue;
using rich_sdna::RichSDNA;
using rich_sdna::Struct;
using rich_sdna::StructMember;
using rich_sdna::Type;

static std::optional<BlendSDNA> prepare_blend_sdna(std::unique_ptr<RichSDNA> rich_sdna)
{
  BlendSDNA blend_sdna;
  blend_sdna.ID = rich_sdna->try_find_struct("ID");
  if (!blend_sdna.ID) {
    return std::nullopt;
  }
  blend_sdna.ID_name = blend_sdna.ID->members.lookup_key_default_as("name", nullptr);
  if (!blend_sdna.ID_name) {
    return std::nullopt;
  }
  if (!blend_sdna.ID_name->is_char_array()) {
    return std::nullopt;
  }
  if (blend_sdna.ID_name->elem_num < 10) {
    return std::nullopt;
  }
  blend_sdna.ListBase = rich_sdna->try_find_struct("ListBase");
  if (!blend_sdna.ListBase) {
    return std::nullopt;
  }
  if (blend_sdna.ListBase->members.size() != 2) {
    return std::nullopt;
  }
  blend_sdna.ListBase_first = blend_sdna.ListBase->members[0];
  if (!blend_sdna.ListBase_first->is_single_pointer()) {
    return std::nullopt;
  }
  blend_sdna.ListBase_last = blend_sdna.ListBase->members[1];
  if (!blend_sdna.ListBase_last->is_single_pointer()) {
    return std::nullopt;
  }
  blend_sdna.sdna = std::move(rich_sdna);
  return blend_sdna;
}

static bool is_specific_id_struct(const BlendSDNA &sdna, const Struct &sdna_struct)
{
  if (sdna_struct.members.is_empty()) {
    return false;
  }
  const StructMember &first_member = *sdna_struct.members[0];
  if (first_member.name != "id") {
    return false;
  }
  if (first_member.type != sdna.ID->type) {
    return false;
  }
  return true;
}

static bool is_top_level_id_block(const BlendBlock &block, const BlendSDNA &sdna)
{
  switch (block.bhead.code) {
    case BLO_CODE_DATA:
    case BLO_CODE_GLOB:
    case BLO_CODE_DNA1:
    case BLO_CODE_TEST:
    case BLO_CODE_REND:
    case BLO_CODE_USER:
    case BLO_CODE_ENDB: {
      return false;
    }
    default: {
      const Struct *sdna_struct = sdna.sdna->try_find_struct(block.bhead.SDNAnr);
      if (!sdna_struct) {
        return false;
      }
      if (!is_specific_id_struct(sdna, *sdna_struct)) {
        return false;
      }
      return true;
    }
  }
}

std::unique_ptr<BlendQuery> BlendQuery::from_file(const StringRef path)
{
  const std::string path_str = path;
  FileReader *reader = BLO_file_reader_uncompressed_from_path(path_str.c_str());
  if (!reader) {
    return nullptr;
  }
  BLI_SCOPED_DEFER([&]() { reader->close(reader); });
  return BlendQuery::from_reader(*reader);
}

static std::optional<StringRefNull> get_c_string_in_buffer(const Span<char> buffer)
{
  const int64_t len = BLI_strnlen(buffer.data(), buffer.size());
  if (len == buffer.size()) {
    return std::nullopt;
  }
  BLI_assert(buffer[len] == '\0');
  return StringRefNull(buffer.data(), len);
}

std::unique_ptr<BlendQuery> BlendQuery::from_reader(FileReader &reader)
{
  std::unique_ptr<BlendQuery> blend = std::make_unique<BlendQuery>();
  ResourceScope &scope = blend->scope_;
  LinearAllocator<> &allocator = scope.allocator();

  const BlenderHeaderVariant header_variant = BLO_readfile_blender_header_decode(&reader);
  const BlenderHeader *header = std::get_if<BlenderHeader>(&header_variant);
  if (!header) {
    return nullptr;
  }
  blend->header_ = *header;
  const BHeadType bhead_type = header->bhead_type();

  std::optional<int64_t> sdna_block_i;
  std::optional<int64_t> global_block_i;
  while (const std::optional<BHead> bhead = BLO_readfile_read_bhead(&reader, bhead_type)) {
    if (bhead->len < 0) {
      return nullptr;
    }
    void *data = allocator.allocate(bhead->len, 16);
    const int64_t read_size = reader.read(&reader, data, bhead->len);
    if (read_size != bhead->len) {
      return nullptr;
    }
    const int64_t index = blend->blocks_.append_and_get_index(
        {*bhead, static_cast<const char *>(data)});
    switch (bhead->code) {
      case BLO_CODE_DNA1: {
        sdna_block_i = index;
        break;
      }
      case BLO_CODE_GLOB: {
        global_block_i = index;
        break;
      }
    }
  }
  if (!sdna_block_i) {
    return nullptr;
  }
  if (!global_block_i) {
    return nullptr;
  }
  const BlendBlock &sdna_block = blend->blocks_[*sdna_block_i];
  std::unique_ptr<rich_sdna::RichSDNA> rich_sdna = rich_sdna::RichSDNA::from_sdna_buffer(
      sdna_block.data, sdna_block.bhead.len);
  if (!rich_sdna) {
    return nullptr;
  }
  std::optional<BlendSDNA> blend_sdna = prepare_blend_sdna(std::move(rich_sdna));
  if (!blend_sdna) {
    return nullptr;
  }
  blend->sdna_ = std::move(*blend_sdna);

  for (const BlendBlock &block : blend->blocks_) {
    if (block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX) {
      continue;
    }
    const Struct *sdna_struct = blend->sdna_.sdna->try_find_struct(block.bhead.SDNAnr);
    if (!sdna_struct) {
      return nullptr;
    }
    const int64_t expected_size = sdna_struct->type->size_in_bytes * block.bhead.nr;
    const int64_t actual_size = block.bhead.len;
    if (expected_size != actual_size) {
      return nullptr;
    }
  }

  {
    int64_t i = 0;
    while (i < blend->blocks_.size()) {
      const BlendBlock &block = blend->blocks_[i];
      if (!is_top_level_id_block(block, blend->sdna_)) {
        i++;
        continue;
      }
      const std::optional<StringRefNull> name_with_prefix = get_c_string_in_buffer(
          {block.data + blend->sdna_.ID_name->offset_in_struct, blend->sdna_.ID_name->elem_num});
      if (!name_with_prefix) {
        return nullptr;
      }
      if (name_with_prefix->size() <= 2) {
        return nullptr;
      }
      const Struct *sdna_struct = blend->sdna_.sdna->try_find_struct(block.bhead.SDNAnr);
      if (!sdna_struct) {
        return nullptr;
      }
      const StringRefNull name = name_with_prefix->c_str() + 2;
      BlendId id;
      id.name = name;
      id.sdna_struct = sdna_struct;
      id.id_block = &block;
      i++;
      const int64_t first_data_i = i;
      while (i < blend->blocks_.size()) {
        const BlendBlock &next_block = blend->blocks_[i];
        if (next_block.bhead.code != BLO_CODE_DATA) {
          break;
        }
        i++;
      }
      id.internal_blocks = blend->blocks_.as_span().slice(
          IndexRange::from_begin_end(first_data_i, i));
      blend->ids_.append(std::move(id));
    }
  }
  return blend;
}

}  // namespace blender::blend_query
