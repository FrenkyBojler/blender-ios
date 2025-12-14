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
struct BlendStruct;
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
};

struct BlendSDNA {
  std::unique_ptr<RichSDNA> sdna;
  const Struct *id_struct = nullptr;
  const StructMember *id_name_member = nullptr;
};

class BlendQuery {
 private:
  ResourceScope scope_;
  BlendSDNA sdna_;
  BlenderHeader header_;
  Vector<BlendBlock> blocks_;
  Vector<BlendId> ids_;

 public:
  static std::unique_ptr<BlendQuery> from_file(StringRef path);
  static std::unique_ptr<BlendQuery> from_reader(FileReader &reader);
};

}  // namespace blender::blend_query
