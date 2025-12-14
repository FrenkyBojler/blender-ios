/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "blend_query.hh"

#include "BLI_color_types.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_timeit.hh"

#include "BLO_core_blend_header.hh"
#include "BLO_core_file_reader.hh"

#include "DNA_attribute_types.h"

namespace blender::blend_query {

using rich_sdna::PrimitiveType;
using rich_sdna::PrimitiveValue;
using rich_sdna::RichSDNA;
using rich_sdna::Struct;
using rich_sdna::StructMember;
using rich_sdna::Type;

uint64_t read_address(const void *data)
{
  return *reinterpret_cast<const uint64_t *>(data);
}

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

  for (BlendBlock &block : blend->blocks_) {
    if (block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX) {
      continue;
    }
    block.sdna_struct = blend->sdna_.sdna->try_find_struct(block.bhead.SDNAnr);
    if (!block.sdna_struct) {
      return nullptr;
    }
    const int64_t expected_size = block.sdna_struct->type->size_in_bytes * block.bhead.nr;
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
  for (BlendId &id : blend->ids_) {
    for (const BlendBlock &block : id.internal_blocks) {
      id.internal_block_by_address.add(uint64_t(block.bhead.old), &block);
    }
    blend->id_by_address_.add(uint64_t(id.id_block->bhead.old), &id);
  }

  blend->gather_raw_buffer_types();
  return blend;
}

void BlendQuery::gather_raw_buffer_types()
{
  for (const BlendId &id : this->ids_) {
    this->gather_raw_buffer_types__struct(&id, *id.sdna_struct, id.id_block->data);
    for (const BlendBlock &block : id.internal_blocks) {
      if (!block.sdna_struct) {
        continue;
      }
      for (const int64_t i : IndexRange(block.bhead.nr)) {
        this->gather_raw_buffer_types__struct(
            &id, *block.sdna_struct, block.data + i * block.sdna_struct->type->size_in_bytes);
      }
    }
  }
}

void BlendQuery::gather_raw_buffer_types__struct(const BlendId *id,
                                                 const Struct &sdna_struct,
                                                 const char *data)
{
  if (sdna_struct.type->name == "Attribute") {
    this->gather_raw_buffer_types__attribute(id, sdna_struct, data);
  }
  for (const StructMember *sdna_member : sdna_struct.members) {
    this->gather_raw_buffer_types__struct_member(
        id, *sdna_member, data + sdna_member->offset_in_struct);
  }
}

static int pointer_level_from_name(const StringRef name)
{
  int64_t level = 0;
  for (const char c : name) {
    if (c == '*') {
      level++;
    }
  }
  return level;
}

static const CPPType *cpp_type_from_attribute_type(const int data_type)
{
  switch (data_type) {
    case int(bke::AttrType::Bool):
      return &CPPType::get<bool>();
    case int(bke::AttrType::Int8):
      return &CPPType::get<int8_t>();
    case int(bke::AttrType::Int16_2D):
      return &CPPType::get<short2>();
    case int(bke::AttrType::Int32):
      return &CPPType::get<int>();
    case int(bke::AttrType::Int32_2D):
      return &CPPType::get<int2>();
    case int(bke::AttrType::Float):
      return &CPPType::get<float>();
    case int(bke::AttrType::Float2):
      return &CPPType::get<float2>();
    case int(bke::AttrType::Float3):
      return &CPPType::get<float3>();
    case int(bke::AttrType::Float4x4):
      return &CPPType::get<float4x4>();
    case int(bke::AttrType::ColorByte):
      return &CPPType::get<blender::ColorGeometry4b>();
    case int(bke::AttrType::ColorFloat):
      return &CPPType::get<blender::ColorGeometry4f>();
    case int(bke::AttrType::Quaternion):
      return &CPPType::get<math::Quaternion>();
    default:
      return nullptr;
  }
}

void BlendQuery::gather_raw_buffer_types__struct_member(const BlendId *id,
                                                        const StructMember &sdna_member,
                                                        const char *data)
{
  switch (sdna_member.category) {
    case rich_sdna::StructMember::Category::Struct: {
      for (const int64_t i : IndexRange(sdna_member.elem_num)) {
        this->gather_raw_buffer_types__struct(
            id, *sdna_member.type->opt_struct, data + i * sdna_member.elem_size);
      }
      break;
    }
    case rich_sdna::StructMember::Category::Primitive: {
      /* Nothing to do because primitive types don't contain pointers. */
      break;
    }
    case rich_sdna::StructMember::Category::Pointer: {
      const int pointer_level = pointer_level_from_name(sdna_member.raw_name);
      BLI_assert(pointer_level >= 1);
      for (const int64_t i : IndexRange(sdna_member.elem_num)) {
        const uint64_t address = read_address(data + i * sdna_member.elem_size);
        if (const BlendBlock *other_block = id->lookup_internal_block(address)) {
          if (other_block->sdna_struct) {
            /* This has a type already. */
            continue;
          }
          RawBufferType raw_buffer_type;
          raw_buffer_type.sdna_base_type = sdna_member.type;
          raw_buffer_type.pointer_level = pointer_level - 1;
          if (raw_buffer_type.pointer_level == 0) {
            raw_buffer_type.elem_size = sdna_member.type->size_in_bytes;
          }
          else {
            raw_buffer_type.elem_size = sdna_.sdna->pointer_size;
          }
          raw_buffer_types_.add(other_block, raw_buffer_type);
        }
      }
      break;
    }
  }
}

void BlendQuery::gather_raw_buffer_types__attribute(const BlendId *id,
                                                    const Struct &sdna_Attribute,
                                                    const char *data)
{
  const std::optional<int> data_type =
      this->lookup(id, sdna_Attribute, data, {"data_type"}).as_primitive<int>();
  const std::optional<int> storage_type =
      this->lookup(id, sdna_Attribute, data, {"storage_type"}).as_primitive<int>();
  if (!data_type || !storage_type) {
    return;
  }
  if (storage_type != int(bke::AttrStorageType::Array)) {
    return;
  }
  const std::optional<uint64_t> array_address =
      this->lookup(id, sdna_Attribute, data, {"data", Deref(), "data"}).as_address();
  if (!array_address) {
    return;
  }
  const BlendBlock *array_block = id->lookup_internal_block(*array_address);
  if (!array_block) {
    return;
  }
  const CPPType *cpp_type = cpp_type_from_attribute_type(*data_type);
  if (!cpp_type) {
    return;
  }
  raw_buffer_types_.add(array_block, RawBufferType::from_cpp_type(*cpp_type));
}

BlendValue BlendQuery::lookup(const BlendId *id,
                              const Struct &sdna_struct,
                              const char *data,
                              const Span<LookupPathElem> &path) const
{
  RawBufferType in_raw_buffer_type;
  in_raw_buffer_type.sdna_base_type = sdna_struct.type;
  in_raw_buffer_type.elem_size = sdna_struct.type->size_in_bytes;
  const BlendValue in = {id, in_raw_buffer_type, 1, data};
  return this->lookup(in, path);
}

BlendValue BlendQuery::lookup(const BlendValue &in, const Span<LookupPathElem> &path) const
{
  BlendValue current = in;
  for (const LookupPathElem &path_elem : path) {
    current = this->lookup(current, path_elem);
    if (current.is_none()) {
      return BlendValue::none();
    }
  }
  return current;
}

BlendValue BlendQuery::lookup(const BlendValue &in, const LookupPathElem &path_elem) const
{
  if (in.is_none()) {
    return BlendValue::none();
  }
  if (const int64_t *index_ptr = std::get_if<int64_t>(&path_elem)) {
    const int64_t index = *index_ptr;
    if (index < 0 || index >= in.size) {
      return BlendValue::none();
    }
    return {in.id, in.type, 1, in.data + in.type.elem_size * index};
  }
  if (const StringRef *member_name_ptr = std::get_if<StringRef>(&path_elem)) {
    const StringRef member_name = *member_name_ptr;
    if (in.type.pointer_level > 0) {
      return BlendValue::none();
    }
    if (!in.type.sdna_base_type) {
      return BlendValue::none();
    }
    if (!in.type.sdna_base_type->opt_struct) {
      return BlendValue::none();
    }
    const StructMember *member = in.type.sdna_base_type->opt_struct->members.lookup_key_default_as(
        member_name, nullptr);
    if (!member) {
      return BlendValue::none();
    }
    switch (member->category) {
      case rich_sdna::StructMember::Category::Struct:
      case rich_sdna::StructMember::Category::Primitive: {
        return {in.id,
                RawBufferType::from_sdna_type(*member->type),
                member->elem_num,
                in.data + member->offset_in_struct};
      }
      case rich_sdna::StructMember::Category::Pointer:
        return {in.id,
                RawBufferType::from_sdna_type(*member->type,
                                              pointer_level_from_name(member->raw_name)),
                member->elem_num,
                in.data + member->offset_in_struct};
    }
  }
  if (std::holds_alternative<Deref>(path_elem)) {
    if (in.type.pointer_level == 0) {
      return BlendValue::none();
    }
    const uint64_t address = read_address(in.data);
    if (in.id) {
      if (const BlendBlock *other_block = in.id->lookup_internal_block(address)) {
        if (in.type.pointer_level == 1) {
          if (other_block->sdna_struct) {
            return {in.id,
                    RawBufferType::from_sdna_type(*other_block->sdna_struct->type),
                    other_block->bhead.nr,
                    other_block->data};
          }
          if (const RawBufferType *raw_buffer_type = this->lookup_raw_buffer_type(*other_block)) {
            if (other_block->bhead.len % raw_buffer_type->elem_size != 0) {
              return BlendValue::none();
            }
            const int64_t elem_num = other_block->bhead.len / raw_buffer_type->elem_size;
            return {in.id, *raw_buffer_type, elem_num, other_block->data};
          }
        }
        if (in.type.pointer_level >= 2) {
          RawBufferType raw_buffer_type = in.type;
          raw_buffer_type.pointer_level--;
          if (other_block->bhead.len % raw_buffer_type.elem_size != 0) {
            return BlendValue::none();
          }
          const int64_t elem_num = other_block->bhead.len / sdna_.sdna->pointer_size;
          return {in.id, raw_buffer_type, elem_num, other_block->data};
        }
      }
      if (const BlendId *other_id = id_by_address_.lookup_default(address, nullptr)) {
        if (in.type.pointer_level != 1) {
          return BlendValue::none();
        }
        return {other_id,
                RawBufferType::from_sdna_type(*other_id->sdna_struct->type),
                1,
                other_id->id_block->data};
      }
    }
  }
  return BlendValue::none();
}

PrimitiveValue read_primitive_value(const PrimitiveType type, const void *data)
{
  switch (type) {
    case PrimitiveType::Char:
      return *reinterpret_cast<const char *>(data);
    case PrimitiveType::UChar:
      return *reinterpret_cast<const uchar *>(data);
    case PrimitiveType::Short:
      return *reinterpret_cast<const short *>(data);
    case PrimitiveType::UShort:
      return *reinterpret_cast<const ushort *>(data);
    case PrimitiveType::Int:
      return *reinterpret_cast<const int *>(data);
    case PrimitiveType::Float:
      return *reinterpret_cast<const float *>(data);
    case PrimitiveType::Double:
      return *reinterpret_cast<const double *>(data);
    case PrimitiveType::Int64:
      return *reinterpret_cast<const int64_t *>(data);
    case PrimitiveType::UInt64:
      return *reinterpret_cast<const uint64_t *>(data);
    case PrimitiveType::Int8:
      return *reinterpret_cast<const int8_t *>(data);
  }
  BLI_assert_unreachable();
  return 0;
}

PrimitiveValue decode_primitive_id_property_value(const eIDPropertyType type,
                                                  const int val,
                                                  const int val2)
{
  union {
    struct {
      int val;
      int val2;
    } encoded;
    int int_value;
    float float_value;
    double double_value;
  } encoded;
  encoded.encoded.val = val;
  encoded.encoded.val2 = val2;
  switch (type) {
    case IDP_INT:
      return encoded.int_value;
    case IDP_FLOAT:
      return encoded.float_value;
    case IDP_DOUBLE:
      return encoded.double_value;
    case IDP_BOOLEAN:
      return encoded.int_value != 0;
    default: {
      BLI_assert_unreachable();
      return {};
    }
  }
}

template<typename T> std::optional<T> BlendValue::as_primitive() const
{
  if (this->is_none()) {
    return std::nullopt;
  }
  if (this->type.pointer_level != 0) {
    return std::nullopt;
  }
  if (this->type.sdna_base_type->opt_primitive_type) {
    const PrimitiveValue value = read_primitive_value(
        *this->type.sdna_base_type->opt_primitive_type, this->data);
    return std::visit([](const auto &v) { return T(v); }, value);
  }
  return std::nullopt;
}

/* Explicit template instantiation. */
template std::optional<float> BlendValue::as_primitive<float>() const;
template std::optional<double> BlendValue::as_primitive<double>() const;
template std::optional<int> BlendValue::as_primitive<int>() const;
template std::optional<char> BlendValue::as_primitive<char>() const;

std::optional<uint64_t> BlendValue::as_address() const
{
  if (this->is_none()) {
    return std::nullopt;
  }
  if (this->type.pointer_level == 0) {
    return std::nullopt;
  }
  return read_address(this->data);
}

std::optional<StringRefNull> BlendValue::as_string() const
{
  if (this->is_none()) {
    return std::nullopt;
  }
  if (this->type.pointer_level != 0) {
    return std::nullopt;
  }
  if (this->size <= 1) {
    return std::nullopt;
  }

  if (this->type.sdna_base_type &&
      this->type.sdna_base_type->opt_primitive_type == PrimitiveType::Char)
  {
    return get_c_string_in_buffer({this->data, this->size});
  }
  return std::nullopt;
}

}  // namespace blender::blend_query
