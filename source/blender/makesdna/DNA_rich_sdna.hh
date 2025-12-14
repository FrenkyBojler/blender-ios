/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>
#include <optional>
#include <variant>

#include "BLI_resource_scope.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_genfile.h"
#include "DNA_sdna_types.h"

namespace blender::rich_sdna {

class Member;
class Struct;
class Type;
class RichSDNA;

using PrimitiveValue =
    std::variant<char, uchar, short, ushort, int, float, double, int64_t, uint64_t>;

class StructMember {
 public:
  /** For consistency with core Blender, this may still contain e.g. the `*` if it is a pointer. */
  StringRefNull identifier;
  /** Same as the identifier but may additionally have an array suffix (e.g. `[3]`). */
  StringRefNull name_with_array;
  StringRefNull name_only;
  int64_t offset_in_struct;
  int64_t elem_size;
  int64_t elem_num;
  int64_t size_in_bytes;
  Struct *parent;

  enum class Category {
    Struct,
    Primitive,
    Pointer,
  };
  Category category;
  const Type *type;

  bool is_char_array() const;
  bool is_single_pointer() const;
  std::string to_decl_string() const;
};

struct StructMemberIdentifierGetter {
  StringRef operator()(const StructMember *member) const
  {
    return member->identifier;
  }
};

class Struct {
 public:
  const Type *type;
  CustomIDVectorSet<const StructMember *, StructMemberIdentifierGetter> members;

  std::optional<int64_t> offset_of(const StringRef name) const
  {
    if (const StructMember *member = this->members.lookup_key_default_as(name, nullptr)) {
      return member->offset_in_struct;
    }
    return std::nullopt;
  }
};

class Type {
 public:
  StringRefNull name;
  int64_t size_in_bytes;
  const Struct *opt_struct = nullptr;
  std::optional<eSDNA_Type> opt_primitive_type = std::nullopt;
  int64_t index;
  const RichSDNA *owner;

  void print(std::ostream &stream, const bool verbose = false) const
  {
    fmt::memory_buffer mem_buf;
    fmt::appender dst{mem_buf};
    fmt::format_to(dst, "Type: {}\n", this->name);
    fmt::format_to(dst, "  Size in bytes: {}\n", this->size_in_bytes);
    if (this->opt_struct) {
      fmt::format_to(dst, "  Members:\n");
      for (const int member_i : this->opt_struct->members.index_range()) {
        const StructMember &member = *this->opt_struct->members[member_i];
        if (verbose) {
          fmt::format_to(dst, "    {}\n", member.identifier);
          fmt::format_to(dst, "      Name with array: {}\n", member.name_with_array);
          fmt::format_to(dst, "      Offset in struct: {}\n", member.offset_in_struct);
          fmt::format_to(dst, "      Elem size: {}\n", member.elem_size);
          fmt::format_to(dst, "      Elem num: {}\n", member.elem_num);
          fmt::format_to(dst, "      Size in bytes: {}\n", member.size_in_bytes);
          fmt::format_to(dst, "      Category: {}\n", int(member.category));
          fmt::format_to(dst, "      Type: {}\n", member.type->name);
        }
        else {
          fmt::format_to(dst, "    {} {}\n", member.type->name, member.name_with_array);
        }
      }
    }
    stream << fmt::to_string(mem_buf);
  }
};

struct StructNameGetter {
  StringRef operator()(const Struct *strct) const
  {
    return strct->type->name;
  }
};

struct TypeNameGetter {
  StringRef operator()(const Type *type_) const
  {
    return type_->name;
  }
};

static bool name_is_pointer(const StringRefNull name)
{
  return name[0] == '*' || (name[0] == '(' && name[1] == '*');
}

static std::string strip_name(const StringRefNull identifier)
{
  std::string result;
  for (const char c : identifier) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
      result.push_back(c);
    }
  }
  return result;
}

class RichSDNA {
 public:
  ResourceScope scope_;
  CustomIDVectorSet<const Struct *, StructNameGetter> structs;
  CustomIDVectorSet<const Type *, TypeNameGetter> types;

  RichSDNA(const SDNA &raw_sdna)
  {
    LinearAllocator<> &allocator = scope_.allocator();
    for (const int type_i : IndexRange(raw_sdna.types_num)) {
      const StringRefNull type_name = allocator.copy_string(raw_sdna.types[type_i]);
      Type &sdna_type = scope_.construct<Type>();
      sdna_type.owner = this;
      sdna_type.name = type_name;
      sdna_type.size_in_bytes = raw_sdna.types_size[type_i];
      sdna_type.index = type_i;
      if (type_name == "char") {
        sdna_type.opt_primitive_type = SDNA_TYPE_CHAR;
        BLI_assert(sdna_type.size_in_bytes == 1);
      }
      else if (type_name == "uchar") {
        sdna_type.opt_primitive_type = SDNA_TYPE_UCHAR;
        BLI_assert(sdna_type.size_in_bytes == 1);
      }
      else if (type_name == "short") {
        sdna_type.opt_primitive_type = SDNA_TYPE_SHORT;
        BLI_assert(sdna_type.size_in_bytes == 2);
      }
      else if (type_name == "ushort") {
        sdna_type.opt_primitive_type = SDNA_TYPE_USHORT;
        BLI_assert(sdna_type.size_in_bytes == 2);
      }
      else if (type_name == "int") {
        sdna_type.opt_primitive_type = SDNA_TYPE_INT;
        BLI_assert(sdna_type.size_in_bytes == 4);
      }
      else if (type_name == "float") {
        sdna_type.opt_primitive_type = SDNA_TYPE_FLOAT;
        BLI_assert(sdna_type.size_in_bytes == 4);
      }
      else if (type_name == "double") {
        sdna_type.opt_primitive_type = SDNA_TYPE_DOUBLE;
        BLI_assert(sdna_type.size_in_bytes == 8);
      }
      else if (type_name == "int64_t") {
        sdna_type.opt_primitive_type = SDNA_TYPE_INT64;
        BLI_assert(sdna_type.size_in_bytes == 8);
      }
      else if (type_name == "uint64_t") {
        sdna_type.opt_primitive_type = SDNA_TYPE_UINT64;
        BLI_assert(sdna_type.size_in_bytes == 8);
      }
      else if (type_name == "int8_t") {
        sdna_type.opt_primitive_type = SDNA_TYPE_INT8;
        BLI_assert(sdna_type.size_in_bytes == 1);
      }

      this->types.add(&sdna_type);
    }
    for (const int struct_i : IndexRange(raw_sdna.structs_num)) {
      const SDNA_Struct &raw_struct = *raw_sdna.structs[struct_i];
      Struct &sdna_struct = scope_.construct<Struct>();
      sdna_struct.type = this->types[raw_struct.type_index];
      const_cast<Type *>(sdna_struct.type)->opt_struct = &sdna_struct;

      for (const int member_i : IndexRange(raw_struct.members_num)) {
        const SDNA_StructMember &raw_member = raw_struct.members[member_i];
        StructMember &sdna_member = scope_.construct<StructMember>();
        sdna_member.elem_num = raw_sdna.members_array_num[raw_member.member_index];
        sdna_member.type = this->types[raw_member.type_index];
        sdna_member.name_with_array = allocator.copy_string(
            raw_sdna.members[raw_member.member_index]);
        const int array_start = sdna_member.name_with_array.find_first_of('[');
        if (array_start == StringRef::not_found) {
          sdna_member.identifier = sdna_member.name_with_array;
        }
        else {
          sdna_member.identifier = allocator.copy_string(
              sdna_member.name_with_array.substr(0, array_start));
        }
        sdna_member.name_only = allocator.copy_string(strip_name(sdna_member.identifier));
        sdna_member.parent = &sdna_struct;
        sdna_struct.members.add(&sdna_member);
      }
      this->structs.add(&sdna_struct);
    }
    for (const Struct *sdna_struct_const : this->structs) {
      Struct &sdna_struct = const_cast<Struct &>(*sdna_struct_const);
      int64_t offset = 0;
      for (const StructMember *sdna_member_const : sdna_struct.members) {
        StructMember &sdna_member = const_cast<StructMember &>(*sdna_member_const);
        sdna_member.offset_in_struct = offset;
        if (name_is_pointer(sdna_member.name_with_array)) {
          sdna_member.category = StructMember::Category::Pointer;
          sdna_member.elem_size = raw_sdna.pointer_size;
        }
        else if (sdna_member.type->opt_struct) {
          sdna_member.category = StructMember::Category::Struct;
          sdna_member.elem_size = sdna_member.type->size_in_bytes;
        }
        else {
          sdna_member.category = StructMember::Category::Primitive;
          sdna_member.elem_size = sdna_member.type->size_in_bytes;
        }
        sdna_member.size_in_bytes = sdna_member.elem_size * sdna_member.elem_num;
        offset += sdna_member.size_in_bytes;
      }
    }
  }

  void print(std::ostream &stream, const bool verbose = false) const
  {
    for (const int type_i : this->types.index_range()) {
      const Type &type = *this->types[type_i];
      type.print(stream, verbose);
    }
  }

  const Struct *try_find_struct(const int struct_nr) const
  {
    if (struct_nr < 0 || struct_nr >= this->structs.size()) {
      return nullptr;
    }
    return this->structs[struct_nr];
  }

  const Struct *try_find_struct(const StringRef name) const
  {
    return this->structs.lookup_key_default_as(name, nullptr);
  }

  const Type *try_find_type(const StringRef name) const
  {
    return this->types.lookup_key_default_as(name, nullptr);
  }
};

bool StructMember::is_char_array() const
{
  if (this->category != Category::Primitive) {
    return false;
  }
  if (this->type->name != "char") {
    return false;
  }
  if (this->elem_num <= 1) {
    return false;
  }
  return true;
}

bool StructMember::is_single_pointer() const
{
  return this->category == StructMember::Category::Pointer && this->elem_num == 1;
}

std::string StructMember::to_decl_string() const
{
  return fmt::format(
      "{} {}::{}", this->type->name, this->parent->type->name, this->name_with_array);
}
}  // namespace blender::rich_sdna
