/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_filereader.h"
#include "BLI_string_ref.hh"

#include "BLO_core_blend_header.hh"

#include "DNA_rich_sdna.hh"

namespace blender::blend_query {

using rich_sdna::PrimitiveType;
using rich_sdna::PrimitiveValue;
using rich_sdna::RichSDNA;
using rich_sdna::Struct;
using rich_sdna::StructMember;
using rich_sdna::Type;

struct BlendBlock;
struct BlendId;
struct BlendSDNA;
class BlendQuery;

struct BlendBlock {
  BHead bhead;
  const char *data = nullptr;
};

struct BlendId {
  std::string name;
  const Struct *sdna_struct = nullptr;
  const BlendBlock *id_block = nullptr;
  Span<BlendBlock> internal_blocks;
  Map<uint64_t, const BlendBlock *> internal_block_by_address;

  const BlendBlock *lookup_internal_block(const uint64_t address) const;
};

struct BlendSDNA {
  std::unique_ptr<RichSDNA> sdna;
  const Struct *ID = nullptr;
  const StructMember *ID_name = nullptr;
  const Struct *ListBase = nullptr;
  const StructMember *ListBase_first = nullptr;
  const StructMember *ListBase_last = nullptr;
};

class BlendQuery {
 private:
  ResourceScope scope_;
  BlendSDNA sdna_;
  BlenderHeader header_;
  Vector<BlendBlock> blocks_;
  Vector<BlendId> ids_;
  Map<uint64_t, const BlendId *> id_by_address_;

 public:
  static std::unique_ptr<BlendQuery> from_file(StringRef path);
  static std::unique_ptr<BlendQuery> from_reader(FileReader &reader);

  const BlendSDNA &sdna() const;
  Span<BlendId> ids() const;

  const BlendId *lookup_id(const uint64_t address) const;
};

inline const BlendSDNA &BlendQuery::sdna() const
{
  return sdna_;
}

inline Span<BlendId> BlendQuery::ids() const
{
  return ids_;
}

inline const BlendId *BlendQuery::lookup_id(const uint64_t address) const
{
  return id_by_address_.lookup_default(address, nullptr);
}

inline const BlendBlock *BlendId::lookup_internal_block(const uint64_t address) const
{
  return this->internal_block_by_address.lookup_default(address, nullptr);
}

}  // namespace blender::blend_query
