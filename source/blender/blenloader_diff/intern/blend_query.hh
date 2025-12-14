/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_filereader.h"
#include "BLI_string_ref.hh"
#include "BLI_struct_equality_utils.hh"

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
  const Struct *sdna_struct = nullptr;
};

struct BlendId {
  std::string name;
  const Struct *sdna_struct = nullptr;
  const BlendBlock *id_block = nullptr;
  Span<BlendBlock> internal_blocks;
  Map<uint64_t, const BlendBlock *> internal_block_by_address;

  const BlendBlock *lookup_internal_block(const uint64_t address) const;
};

struct RawBufferType {
  const Type *sdna_base_type = nullptr;
  const CPPType *cpp_base_type = nullptr;
  int pointer_level = 0;
  int elem_size = 0;

  static RawBufferType from_sdna_type(const Type &sdna_type, const int pointer_level = 0)
  {
    RawBufferType raw_buffer_type;
    raw_buffer_type.sdna_base_type = &sdna_type;
    raw_buffer_type.elem_size = sdna_type.size_in_bytes;
    raw_buffer_type.pointer_level = pointer_level;
    return raw_buffer_type;
  }

  static RawBufferType from_cpp_type(const CPPType &cpp_type, const int pointer_level = 0)
  {
    RawBufferType raw_buffer_type;
    raw_buffer_type.cpp_base_type = &cpp_type;
    raw_buffer_type.elem_size = cpp_type.size;
    raw_buffer_type.pointer_level = pointer_level;
    return raw_buffer_type;
  }

  BLI_STRUCT_EQUALITY_OPERATORS_3(RawBufferType, sdna_base_type, cpp_base_type, pointer_level)
};

struct BlendSDNA {
  std::unique_ptr<RichSDNA> sdna;
  const Struct *ID = nullptr;
  const StructMember *ID_name = nullptr;
  const Struct *ListBase = nullptr;
  const StructMember *ListBase_first = nullptr;
  const StructMember *ListBase_last = nullptr;
};

struct Deref {};

using LookupPathElem = std::variant<int64_t, StringRef, Deref>;

struct BlendValue {
  const BlendId *id = nullptr;
  RawBufferType type;
  int64_t size = 0;
  const char *data = nullptr;

  static BlendValue none()
  {
    return {};
  }

  bool is_none() const
  {
    return this->data == nullptr;
  }

  template<typename T> std::optional<T> as_primitive() const;
  std::optional<uint64_t> as_address() const;
};

class BlendQuery {
 private:
  ResourceScope scope_;
  BlendSDNA sdna_;
  BlenderHeader header_;
  Vector<BlendBlock> blocks_;
  Vector<BlendId> ids_;
  Map<uint64_t, const BlendId *> id_by_address_;
  Map<const BlendBlock *, RawBufferType> raw_buffer_types_;

 public:
  static std::unique_ptr<BlendQuery> from_file(StringRef path);
  static std::unique_ptr<BlendQuery> from_reader(FileReader &reader);

  const BlendSDNA &sdna() const;
  Span<BlendId> ids() const;

  const BlendId *lookup_id(const uint64_t address) const;
  const RawBufferType *lookup_raw_buffer_type(const BlendBlock &block) const;

  BlendValue lookup(const BlendValue &in, const LookupPathElem &path_elem) const;
  BlendValue lookup(const BlendId *id,
                    const Struct &sdna_struct,
                    const char *data,
                    const Span<LookupPathElem> &path) const;

 private:
  void gather_raw_buffer_types();
  void gather_raw_buffer_types__struct(const BlendId *id,
                                       const Struct &sdna_struct,
                                       const char *data);
  void gather_raw_buffer_types__struct_member(const BlendId *id,
                                              const StructMember &sdna_member,
                                              const char *data);
  void gather_raw_buffer_types__attribute(const BlendId *id,
                                          const Struct &sdna_Attribute,
                                          const char *data);
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

inline const RawBufferType *BlendQuery::lookup_raw_buffer_type(const BlendBlock &block) const
{
  return raw_buffer_types_.lookup_ptr(&block);
}

}  // namespace blender::blend_query
