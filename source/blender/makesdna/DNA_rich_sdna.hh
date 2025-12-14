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

  void print(std::ostream &stream, bool verbose = false) const;
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

class RichSDNA {
 public:
  ResourceScope scope_;
  CustomIDVectorSet<const Struct *, StructNameGetter> structs;
  CustomIDVectorSet<const Type *, TypeNameGetter> types;

  static std::unique_ptr<RichSDNA> from_sdna(const SDNA &raw_sdna);
  static std::unique_ptr<RichSDNA> from_sdna_buffer(const void *buffer, const int64_t buffer_size);

  void print(std::ostream &stream, const bool verbose = false) const;
  const Struct *try_find_struct(const int struct_nr) const;
  const Struct *try_find_struct(const StringRef name) const;
  const Type *try_find_type(const StringRef name) const;
};

inline bool StructMember::is_char_array() const
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

inline bool StructMember::is_single_pointer() const
{
  return this->category == StructMember::Category::Pointer && this->elem_num == 1;
}

}  // namespace blender::rich_sdna
