#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <xxhash.h>

#include "BLI_filereader.h"
#include "BLI_index_range.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_resource_scope.hh"
#include "BLI_set.hh"
#include "BLI_stack.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_struct_equality_utils.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"
#include "BLO_core_blend_header.hh"
#include "BLO_core_file_reader.hh"

#include "DNA_genfile.h"
#include "DNA_node_types.h"
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

namespace blender::blend_diff {

using rich_sdna::PrimitiveValue;
using rich_sdna::RichSDNA;
using rich_sdna::Struct;
using rich_sdna::StructMember;
using rich_sdna::Type;

struct DiffOptions {
  ResourceScope scope_;
  bool ignore_pad = true;

  struct MemberName {
    StringRef type_name;
    StringRef member_identifier;

    MemberName(const StringRef type_name, const StringRef member_name)
        : type_name(type_name), member_identifier(member_name)
    {
    }

    MemberName(const StructMember &member)
        : type_name(member.parent->type->name), member_identifier(member.identifier)
    {
    }

    uint64_t hash() const
    {
      return get_default_hash(this->type_name, this->member_identifier);
    }

    BLI_STRUCT_EQUALITY_OPERATORS_2(MemberName, type_name, member_identifier)
  };

  Set<MemberName> members_to_ignore_set;
  Set<std::string> id_types_to_ignore;
  Map<MemberName, uint64_t> ignored_flags;

  void add_member_to_ignore(const StringRef type_name, const StringRef member_name)
  {
    this->add_members_to_ignore(type_name, {member_name});
  }

  void add_members_to_ignore(const StringRef type_name, const Span<StringRef> member_names)
  {
    LinearAllocator<> &allocator = scope_.allocator();
    for (const StringRef member_name : member_names) {
      members_to_ignore_set.add(
          {allocator.copy_string(type_name), allocator.copy_string(member_name)});
    }
  }

  void add_next_prev_ignore_types(const Span<StringRef> type_names)
  {
    for (const StringRef type_name : type_names) {
      this->add_members_to_ignore(type_name, {"*next", "*prev"});
    }
  }

  void add_id_types_to_ignore(const Span<StringRef> type_names)
  {
    for (const StringRef type_name : type_names) {
      this->id_types_to_ignore.add(type_name);
    }
  }

  void add_ignored_flags(const StringRef type_name,
                         const StringRef member_name,
                         const uint64_t flag)
  {
    this->ignored_flags.add({type_name, member_name}, flag);
  }

  bool ignore_member(const StructMember &member) const
  {
    if (this->ignore_pad) {
      if (member.name_only.startswith("_pad")) {
        return true;
      }
    }
    if (this->members_to_ignore_set.contains(member)) {
      return true;
    }
    return false;
  }

  bool ignore_id_type(const StringRef type_name) const
  {
    return this->id_types_to_ignore.contains(type_name);
  }

  uint64_t lookup_ignored_flags(const StructMember &member) const
  {
    return this->ignored_flags.lookup_default(member, 0);
  }
};

class DiffWriter {
 private:
  fmt::memory_buffer mem_buf_;
  fmt::appender dst_{mem_buf_};

 public:
  DiffWriter(StringRef path)
  {
    if (path.startswith("/")) {
      path = path.drop_prefix(1);
    }
    fmt::format_to(dst_, "diff --git a/{} b/{}\n", path, path);
    fmt::format_to(dst_, "--- a/{}\n", path);
    fmt::format_to(dst_, "+++ b/{}\n", path);
    fmt::format_to(dst_, "@@ -1 +100000 @@\n");
  }

  void writeln_unchanged(const StringRef line)
  {
    fmt::format_to(dst_, " {}\n", line);
  }

  void writeln_if_changed(const StringRef old_line, const StringRef new_line)
  {
    if (old_line != new_line) {
      this->writeln_changed(old_line, new_line);
    }
  }

  void writeln_changed(const StringRef old_line, const StringRef new_line)
  {
    this->writeln_removed(old_line);
    this->writeln_added(new_line);
  }

  void writeln_added(const StringRef line)
  {
    fmt::format_to(dst_, "+{}\n", line);
  }

  void writeln_removed(const StringRef line)
  {
    fmt::format_to(dst_, "-{}\n", line);
  }

  std::string to_string() const
  {
    return fmt::to_string(mem_buf_);
  }
};

static void write_diff_struct_member(DiffWriter &writer,
                                     const StructMember &old_member,
                                     const StructMember &new_member)
{
  const std::string old_member_str = old_member.to_decl_string();
  const std::string new_member_str = new_member.to_decl_string();
  if (old_member_str == new_member_str) {
    return;
  }
  writer.writeln_changed(old_member_str, new_member_str);
}

static void write_diff_type(DiffWriter &writer,
                            const DiffOptions &options,
                            const Type &old_type,
                            const Type &new_type)
{
  const StringRef name = old_type.name;
  if (old_type.opt_struct && !new_type.opt_struct) {
    writer.writeln_changed(fmt::format("Type `{}` has struct", name),
                           fmt::format("Type `{}` has no struct", name));
    return;
  }
  if (!old_type.opt_struct && new_type.opt_struct) {
    writer.writeln_changed(fmt::format("Type `{}` has no struct", name),
                           fmt::format("Type `{}` has struct", name));
    return;
  }
  if (!old_type.opt_struct) {
    if (old_type.size_in_bytes != new_type.size_in_bytes) {
      writer.writeln_changed(fmt::format("Type `{}` has size {}", name, old_type.size_in_bytes),
                             fmt::format("Type `{}` has size {}", name, new_type.size_in_bytes));
    }
    return;
  }
  for (const StructMember *old_member : old_type.opt_struct->members) {
    if (options.ignore_member(*old_member)) {
      continue;
    }
    if (const StructMember *new_member = new_type.opt_struct->members.lookup_key_default_as(
            old_member->identifier, nullptr))
    {
      write_diff_struct_member(writer, *old_member, *new_member);
    }
    else {
      writer.writeln_removed(old_member->to_decl_string());
    }
  }
  for (const StructMember *new_member : new_type.opt_struct->members) {
    if (options.ignore_member(*new_member)) {
      continue;
    }
    const StructMember *old_member = old_type.opt_struct->members.lookup_key_default_as(
        new_member->identifier, nullptr);
    if (old_member) {
      continue;
    }
    writer.writeln_added(new_member->to_decl_string());
  }
}

static void write_diff_sdna(DiffWriter &writer,
                            const DiffOptions &options,
                            const RichSDNA &old_sdna,
                            const RichSDNA &new_sdna)
{
  writer.writeln_unchanged("SDNA Changes:");
  for (const Type *old_type : old_sdna.types) {
    if (const Type *new_type = new_sdna.types.lookup_key_default_as(old_type->name, nullptr)) {
      write_diff_type(writer, options, *old_type, *new_type);
    }
    else {
      writer.writeln_removed(fmt::format("Type: {}", old_type->name));
    }
  }
  for (const Type *new_type : new_sdna.types) {
    const Type *old_type = old_sdna.types.lookup_key_default_as(new_type->name, nullptr);
    if (old_type) {
      continue;
    }
    writer.writeln_added(fmt::format("Type: {}", new_type->name));
  }
}

struct BlendBlock {
  BHead bhead;
  const char *data = nullptr;
};

struct DataWithStruct {
  const void *data = nullptr;
  const Struct *sdna_struct = nullptr;
};

struct BlendData {
  std::unique_ptr<LinearAllocator<>> allocator;
  BlenderHeader header;
  Vector<BlendBlock> blocks;
  int64_t sdna_block_index = -1;
  int64_t global_block_index = -1;
};

struct BlendIdData {
  std::string name;
  std::string type_name;
  const BlendBlock *id_block = nullptr;
  Span<BlendBlock> blocks;
};

struct AddressMap {
  Map<uint64_t, const BlendBlock *> map;
};

struct IdAddressMap {
  Map<uint64_t, const BlendIdData *> map;
};

struct BlockMatch {
  const BlendBlock *old_block = nullptr;
  const BlendBlock *new_block = nullptr;
  std::string context;
};

struct BlockMatchMap {
  Map<const BlendBlock *, const BlendBlock *> old_by_new;
  Map<const BlendBlock *, const BlendBlock *> new_by_old;

  bool add(const BlendBlock *old_block, const BlendBlock *new_block)
  {
    const bool newly_added_1 = this->old_by_new.add(new_block, old_block);
    const bool newly_added_2 = this->new_by_old.add(old_block, new_block);
    // TODO
    // BLI_assert(newly_added_1 == newly_added_2);
    return newly_added_1 && newly_added_2;
  }
};

static AddressMap build_address_map(const BlendIdData &id_data)
{
  AddressMap address_map;
  address_map.map.add(uint64_t(id_data.id_block->bhead.old), id_data.id_block);
  for (const BlendBlock &block : id_data.blocks) {
    address_map.map.add(uint64_t(block.bhead.old), &block);
  }
  return address_map;
}

static PrimitiveValue read_primitive_value_at_address(const eSDNA_Type type, const void *data)
{
  switch (type) {
    case SDNA_TYPE_CHAR:
      return *reinterpret_cast<const char *>(data);
    case SDNA_TYPE_UCHAR:
      return *reinterpret_cast<const uchar *>(data);
    case SDNA_TYPE_SHORT:
      return *reinterpret_cast<const short *>(data);
    case SDNA_TYPE_USHORT:
      return *reinterpret_cast<const ushort *>(data);
    case SDNA_TYPE_INT:
      return *reinterpret_cast<const int *>(data);
    case SDNA_TYPE_FLOAT:
      return *reinterpret_cast<const float *>(data);
    case SDNA_TYPE_DOUBLE:
      return *reinterpret_cast<const double *>(data);
    case SDNA_TYPE_INT64:
      return *reinterpret_cast<const int64_t *>(data);
    case SDNA_TYPE_UINT64:
      return *reinterpret_cast<const uint64_t *>(data);
    case SDNA_TYPE_INT8:
      return *reinterpret_cast<const int8_t *>(data);
    case SDNA_TYPE_RAW_DATA:
      break;
  }
  BLI_assert_unreachable();
  return 0;
}

static uint64_t read_address_at_address(const void *data)
{
  return *reinterpret_cast<const uint64_t *>(data);
}

static std::optional<std::string> try_read_inline_string_member(const void *struct_data,
                                                                const Struct &sdna_struct,
                                                                const StringRef member_name)
{
  const StructMember *member = sdna_struct.members.lookup_key_default_as(member_name, nullptr);
  if (!member) {
    return std::nullopt;
  }
  if (member->category != StructMember::Category::Primitive) {
    return std::nullopt;
  }
  if (member->type->opt_primitive_type != SDNA_TYPE_CHAR) {
    return std::nullopt;
  }
  const char *str_data = reinterpret_cast<const char *>(struct_data) + member->offset_in_struct;
  const int64_t len = BLI_strnlen(str_data, member->elem_num);
  return std::string(str_data, len);
}

static std::optional<int> try_read_inline_int_member(const void *struct_data,
                                                     const Struct &sdna_struct,
                                                     const StringRef member_name)
{
  const StructMember *member = sdna_struct.members.lookup_key_default_as(member_name, nullptr);
  if (!member) {
    return std::nullopt;
  }
  if (member->category != StructMember::Category::Primitive) {
    return std::nullopt;
  }
  if (member->type->opt_primitive_type != SDNA_TYPE_INT) {
    return std::nullopt;
  }
  const int value = *reinterpret_cast<const int *>(static_cast<const char *>(struct_data) +
                                                   member->offset_in_struct);
  return value;
}

static std::optional<char> try_read_inline_char_member(const void *struct_data,
                                                       const Struct &sdna_struct,
                                                       const StringRef member_name)
{
  const StructMember *member = sdna_struct.members.lookup_key_default_as(member_name, nullptr);
  if (!member) {
    return std::nullopt;
  }
  if (member->category != StructMember::Category::Primitive) {
    return std::nullopt;
  }
  if (member->type->opt_primitive_type != SDNA_TYPE_CHAR) {
    return std::nullopt;
  }
  const char value = *(static_cast<const char *>(struct_data) + member->offset_in_struct);
  return value;
}

static std::string primitive_value_to_string(const PrimitiveValue &value)
{
  return std::visit([](const auto &v) { return std::to_string(v); }, value);
}

static std::optional<std::string> try_convert_char_array_to_readable_string(const Span<char> chars)
{
  const int64_t len = chars.first_index_try('\0');
  if (len == -1) {
    return std::nullopt;
  }
  const int64_t invalid_index = BLI_str_utf8_invalid_byte(chars.data(), len);
  if (invalid_index != -1) {
    return std::nullopt;
  }
  return std::string(chars.data(), len);
}

struct RawBufferType {
  const Type *base_type;
  int pointer_level = 0;

  BLI_STRUCT_EQUALITY_OPERATORS_2(RawBufferType, base_type, pointer_level)
};

class IdDiffer {
 private:
  DiffWriter &writer_;
  const DiffOptions &options_;

  struct PerBlendData {
    const IdAddressMap &id_addresses;
    const BlendIdData &id_data;
    const RichSDNA &sdna;
    AddressMap addresses;
    Map<const BlendBlock *, RawBufferType> raw_buffer_types;
  };

  PerBlendData old_;
  PerBlendData new_;

  Map<const BlendBlock *, const BlendBlock *> old_by_new_;
  Map<const BlendBlock *, const BlendBlock *> new_by_old_;

  Stack<BlockMatch> matches_to_process_;

  struct Pointee {
    const BlendBlock *block = nullptr;
    const BlendIdData *id_data = nullptr;

    Pointee() = default;
    Pointee(const BlendBlock &block) : block(&block) {}
    Pointee(const BlendIdData &id_data) : block(id_data.id_block), id_data(&id_data) {}

    bool is_local() const
    {
      return this->id_data == nullptr;
    }

    operator bool() const
    {
      return this->block != nullptr;
    }
  };

 public:
  IdDiffer(DiffWriter &writer,
           const DiffOptions &options,
           const BlendIdData &old_id_data,
           const BlendIdData &new_id_data,
           const IdAddressMap &old_ids,
           const IdAddressMap &new_ids,
           const RichSDNA &old_sdna,
           const RichSDNA &new_sdna)
      : writer_(writer),
        options_(options),
        old_{old_ids, old_id_data, old_sdna},
        new_{new_ids, new_id_data, new_sdna}
  {
  }

  void run()
  {
    old_.addresses = build_address_map(old_.id_data);
    new_.addresses = build_address_map(new_.id_data);

    const std::string root_context = fmt::format(
        "{}[\"{}\"]", new_.id_data.type_name, new_.id_data.name.c_str() + 2);
    matches_to_process_.push({old_.id_data.id_block, new_.id_data.id_block, root_context});

    this->gather_raw_buffer_types__blend(old_);
    this->gather_raw_buffer_types__blend(new_);

    while (!matches_to_process_.is_empty()) {
      const BlockMatch match = matches_to_process_.pop();

      if (match.old_block->bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX &&
          match.new_block->bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX)
      {
        this->diff_raw_buffer(*match.old_block, *match.new_block, match.context);
        continue;
      }

      const Struct *old_struct = old_.sdna.try_find_struct(match.old_block->bhead.SDNAnr);
      const Struct *new_struct = new_.sdna.try_find_struct(match.new_block->bhead.SDNAnr);
      if (!old_struct || !new_struct) {
        continue;
      }
      if (match.old_block->bhead.nr == 1 && match.new_block->bhead.nr == 1) {
        this->diff_struct(
            *match.old_block, *match.new_block, 0, 0, *old_struct, *new_struct, match.context);
      }
      else {
        this->diff_struct_array(*match.old_block,
                                *match.new_block,
                                0,
                                0,
                                *old_struct,
                                *new_struct,
                                match.old_block->bhead.nr,
                                match.new_block->bhead.nr,
                                match.context);
      }
    }
  }

  void gather_raw_buffer_types__blend(PerBlendData &blend_data)
  {
    const Struct &id_struct = *blend_data.sdna.try_find_struct(blend_data.id_data.type_name);
    this->gather_raw_buffer_types__struct(blend_data, *blend_data.id_data.id_block, 0, id_struct);
    for (const BlendBlock &block : blend_data.id_data.blocks) {
      const Struct *sdna_struct = blend_data.sdna.try_find_struct(block.bhead.SDNAnr);
      if (!sdna_struct) {
        continue;
      }
      for (const int64_t i : IndexRange(block.bhead.nr)) {
        this->gather_raw_buffer_types__struct(
            blend_data, block, i * sdna_struct->type->size_in_bytes, *sdna_struct);
      }
    }
  }

  void gather_raw_buffer_types__struct(PerBlendData &blend_data,
                                       const BlendBlock &block,
                                       const int64_t struct_offset,
                                       const Struct &sdna_struct)
  {
    for (const StructMember *member : sdna_struct.members) {
      this->gather_raw_buffer_types__struct_member(
          blend_data, block, struct_offset + member->offset_in_struct, *member);
    }
  }

  void gather_raw_buffer_types__struct_member(PerBlendData &blend_data,
                                              const BlendBlock &block,
                                              const int64_t member_offset,
                                              const StructMember &sdna_member)
  {
    switch (sdna_member.category) {
      case rich_sdna::StructMember::Category::Struct: {
        for (const int64_t i : IndexRange(sdna_member.elem_num)) {
          const int64_t struct_offset = member_offset + i * sdna_member.elem_size;
          this->gather_raw_buffer_types__struct(
              blend_data, block, struct_offset, *sdna_member.type->opt_struct);
        }
        break;
      }
      case rich_sdna::StructMember::Category::Primitive: {
        /* Nothing to do because primitive types don't contain pointers. */
        break;
      }
      case rich_sdna::StructMember::Category::Pointer: {
        const int pointer_level = this->pointer_level_from_name(sdna_member.name_with_array);
        BLI_assert(pointer_level >= 1);
        for (const int64_t i : IndexRange(sdna_member.elem_num)) {
          const int64_t offset = member_offset + i * sdna_member.elem_size;
          const uint64_t address = read_address_at_address(block.data + offset);
          if (const BlendBlock *other_block = blend_data.addresses.map.lookup_default(address,
                                                                                      nullptr))
          {
            if (other_block->bhead.SDNAnr != SDNA_RAW_DATA_STRUCT_INDEX) {
              continue;
            }
            RawBufferType raw_buffer_type;
            raw_buffer_type.base_type = sdna_member.type;
            raw_buffer_type.pointer_level = pointer_level - 1;
            blend_data.raw_buffer_types.add(other_block, raw_buffer_type);
          }
        }
        break;
      }
    }
  }

  int pointer_level_from_name(const StringRef name) const
  {
    return name.find_first_not_of('*');
  }

  void diff_raw_buffer(const BlendBlock &old_block,
                       const BlendBlock &new_block,
                       const StringRef context)
  {
    const RawBufferType *old_raw_type = old_.raw_buffer_types.lookup_ptr(&old_block);
    const RawBufferType *new_raw_type = new_.raw_buffer_types.lookup_ptr(&new_block);
    if (!old_raw_type || !new_raw_type) {
      return;
    }
    const int pointer_level = old_raw_type->pointer_level;
    if (pointer_level != new_raw_type->pointer_level) {
      return;
    }
    const Type &old_sdna_type = *old_raw_type->base_type;
    const Type &new_sdna_type = *new_raw_type->base_type;
    if (old_sdna_type.name != new_sdna_type.name) {
      return;
    }
    if (pointer_level == 0) {
      if (ELEM(0, old_sdna_type.size_in_bytes, new_sdna_type.size_in_bytes)) {
        return;
      }
      if (old_block.bhead.len % old_sdna_type.size_in_bytes != 0) {
        return;
      }
      if (new_block.bhead.len % new_sdna_type.size_in_bytes != 0) {
        return;
      }
      const int64_t old_size = old_block.bhead.len / old_sdna_type.size_in_bytes;
      const int64_t new_size = new_block.bhead.len / new_sdna_type.size_in_bytes;
      // TODO: handle float buffers etc
      return;
    }
    if (pointer_level == 1) {
      const int pointer_size = sizeof(void *);
      if (old_block.bhead.len % pointer_size != 0) {
        return;
      }
      if (new_block.bhead.len % pointer_size != 0) {
        return;
      }
      const int64_t old_num = old_block.bhead.len / pointer_size;
      const int64_t new_num = new_block.bhead.len / pointer_size;

      Vector<Pointee> old_pointees;
      Vector<Pointee> new_pointees;
      for (const int64_t i : IndexRange(old_num)) {
        const uint64_t address = read_address_at_address(old_block.data + i * pointer_size);
        old_pointees.append(this->lookup_pointee(old_, address));
      }
      for (const int64_t i : IndexRange(new_num)) {
        const uint64_t address = read_address_at_address(new_block.data + i * pointer_size);
        new_pointees.append(this->lookup_pointee(new_, address));
      }
      this->diff_block_list(old_pointees, new_pointees, context);
      return;
    }
  }

  void diff_struct(const BlendBlock &old_block,
                   const BlendBlock &new_block,
                   const int64_t old_struct_offset,
                   const int64_t new_struct_offset,
                   const Struct &old_struct,
                   const Struct &new_struct,
                   const StringRef context)
  {
    const StringRef type_name = old_struct.type->name;
    if (type_name != new_struct.type->name) {
      writer_.writeln_changed(fmt::format("{} <type> = {}", context, old_struct.type->name),
                              fmt::format("{} <type> = {}", context, new_struct.type->name));
      return;
    }
    if (type_name == "ListBase") {
      this->diff_ListBase(old_block, new_block, old_struct_offset, new_struct_offset, context);
      return;
    }
    if (type_name == "IDProperty") {
      /* IDProperty has some special handling because of how IDPropertyData.val/val2 also encode
       * float/double values. */
      this->diff_IDProperty(old_block,
                            new_block,
                            old_struct_offset,
                            new_struct_offset,
                            old_struct,
                            new_struct,
                            context);
    }

    for (const StructMember *old_member : old_struct.members) {
      const StructMember *new_member = new_struct.members.lookup_key_default_as(
          old_member->identifier, nullptr);
      if (!new_member) {
        /* This is in the diff as part of SDNA changes, no need to mention it for every block. */
        continue;
      }
      this->diff_struct_member(old_block,
                               new_block,
                               old_struct_offset + old_member->offset_in_struct,
                               new_struct_offset + new_member->offset_in_struct,
                               *old_member,
                               *new_member,
                               context);
    }
  }

  void diff_struct_member(const BlendBlock &old_block,
                          const BlendBlock &new_block,
                          const int64_t old_member_offset,
                          const int64_t new_member_offset,
                          const StructMember &old_member,
                          const StructMember &new_member,
                          const StringRef context)
  {
    const StringRef type_name = old_member.type->name;
    const StructMember::Category category = old_member.category;
    const int64_t elem_num = old_member.elem_num;
    const StringRef name_only = old_member.name_only;
    const std::optional<eSDNA_Type> opt_primitive_type = old_member.type->opt_primitive_type;
    if (new_member.type->name != type_name || new_member.category != category ||
        new_member.elem_num != elem_num || new_member.name_only != name_only ||
        new_member.type->opt_primitive_type != opt_primitive_type)
    {
      /* This is in the diff as part of SDNA changes, no need to mention it for every block. */
      return;
    }
    if (options_.ignore_member(new_member)) {
      return;
    }
    const bool is_array = elem_num > 1;
    switch (category) {
      case rich_sdna::StructMember::Category::Struct: {
        const Struct &old_substruct = *old_member.type->opt_struct;
        const Struct &new_substruct = *new_member.type->opt_struct;
        for (const int i : IndexRange(elem_num)) {
          const std::string sub_context = is_array ?
                                              fmt::format("{}.{}[{}]", context, name_only, i) :
                                              fmt::format("{}.{}", context, name_only);
          this->diff_struct(old_block,
                            new_block,
                            old_member_offset + i * old_member.elem_size,
                            new_member_offset + i * new_member.elem_size,
                            old_substruct,
                            new_substruct,
                            sub_context);
        }
        break;
      }
      case rich_sdna::StructMember::Category::Primitive: {
        BLI_assert(opt_primitive_type.has_value());
        const eSDNA_Type primitive_type = *opt_primitive_type;
        if (primitive_type == SDNA_TYPE_CHAR) {
          const Span<char> old_values{old_block.data + old_member_offset, elem_num};
          const Span<char> new_values{new_block.data + new_member_offset, elem_num};
          const std::optional<std::string> old_str = try_convert_char_array_to_readable_string(
              old_values);
          const std::optional<std::string> new_str = try_convert_char_array_to_readable_string(
              new_values);
          if (old_str && new_str) {
            if (old_str == new_str) {
              break;
            }
            writer_.writeln_changed(fmt::format("{}.{} = {}", context, name_only, *old_str),
                                    fmt::format("{}.{} = {}", context, name_only, *new_str));
            break;
          }
        }
        for (const int i : IndexRange(elem_num)) {
          const PrimitiveValue old_value = read_primitive_value_at_address(
              primitive_type, old_block.data + old_member_offset + i * old_member.elem_size);
          const PrimitiveValue new_value = read_primitive_value_at_address(
              primitive_type, new_block.data + new_member_offset + i * new_member.elem_size);

          const uint64_t ignored_flags = options_.lookup_ignored_flags(new_member);
          if (ignored_flags != 0) {
            const uint64_t old_flags = std::visit([](const auto &v) { return uint64_t(v); },
                                                  old_value);
            const uint64_t new_flags = std::visit([](const auto &v) { return uint64_t(v); },
                                                  new_value);
            if ((old_flags & ~ignored_flags) == (new_flags & ~ignored_flags)) {
              continue;
            }
          }

          if (old_value == new_value) {
            continue;
          }
          const std::string sub_context = is_array ?
                                              fmt::format("{}.{}[{}]", context, name_only, i) :
                                              fmt::format("{}.{}", context, name_only);
          writer_.writeln_changed(
              fmt::format("{} = {}", sub_context, primitive_value_to_string(old_value)),
              fmt::format("{} = {}", sub_context, primitive_value_to_string(new_value)));
        }
        break;
      }
      case rich_sdna::StructMember::Category::Pointer: {
        for (const int i : IndexRange(elem_num)) {
          const uint64_t old_address = read_address_at_address(old_block.data + old_member_offset +
                                                               i * old_member.elem_size);
          const uint64_t new_address = read_address_at_address(new_block.data + new_member_offset +
                                                               i * new_member.elem_size);
          const Pointee old_pointee = this->lookup_pointee(old_, old_address);
          const Pointee new_pointee = this->lookup_pointee(new_, new_address);
          if (!old_pointee && !new_pointee) {
            continue;
          }
          const std::string sub_context = is_array ?
                                              fmt::format("{}.{}[{}]", context, name_only, i) :
                                              fmt::format("{}.{}", context, name_only);
          PointeeToStringOptions options;
          options.include_identifier = true;
          const std::string old_line = fmt::format(
              "{} = {}", sub_context, this->pointee_to_string(old_, old_pointee, options));
          const std::string new_line = fmt::format(
              "{} = {}", sub_context, this->pointee_to_string(new_, new_pointee, options));
          if (old_line != new_line) {
            writer_.writeln_changed(old_line, new_line);
          }
          if (old_pointee.id_data || new_pointee.id_data) {
            continue;
          }
          if (old_pointee.block && new_pointee.block) {
            this->tag_potentially_corresponding_blocks(
                *old_pointee.block, *new_pointee.block, sub_context);
          }
        }
        break;
      }
    }
  }

  void diff_ListBase(const BlendBlock &old_block,
                     const BlendBlock &new_block,
                     const int64_t old_struct_offset,
                     const int64_t new_struct_offset,
                     const StringRef context)
  {
    const uint64_t old_first_address = *reinterpret_cast<const uint64_t *>(old_block.data +
                                                                           old_struct_offset);
    const uint64_t new_first_address = *reinterpret_cast<const uint64_t *>(new_block.data +
                                                                           new_struct_offset);
    const Vector<Pointee> old_pointees = this->gather_linked_list_pointees(old_first_address,
                                                                           old_);
    const Vector<Pointee> new_pointees = this->gather_linked_list_pointees(new_first_address,
                                                                           new_);
    this->diff_block_list(old_pointees, new_pointees, context);
  }

  void diff_struct_array(const BlendBlock &old_block,
                         const BlendBlock &new_block,
                         const int64_t old_offset,
                         const int64_t new_offset,
                         const Struct &old_struct,
                         const Struct &new_struct,
                         const int64_t old_num,
                         const int64_t new_num,
                         const StringRef context)
  {
    Vector<DataWithStruct> old_structs;
    Vector<DataWithStruct> new_structs;
    for (const int64_t i : IndexRange(old_num)) {
      old_structs.append(
          {old_block.data + old_offset + i * old_struct.type->size_in_bytes, &old_struct});
    }
    for (const int64_t i : IndexRange(new_num)) {
      new_structs.append(
          {new_block.data + new_offset + i * new_struct.type->size_in_bytes, &new_struct});
    }
    this->diff_struct_list(old_block, new_block, old_structs, new_structs, context);
  }

  void diff_struct_list(const BlendBlock &old_block,
                        const BlendBlock &new_block,
                        const Span<DataWithStruct> old_structs,
                        const Span<DataWithStruct> new_structs,
                        const StringRef context)
  {
    struct Item {
      DataWithStruct data_with_struct;
      int64_t index;
    };

    Map<std::string, Item> old_struct_map;
    for (const int64_t i : old_structs.index_range()) {
      const DataWithStruct &old_struct = old_structs[i];
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_struct.data, *old_struct.sdna_struct, i);
      if (!old_struct_map.add(identifier, {old_struct, i})) {
        const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
            old_, old_struct.data, *old_struct.sdna_struct, i, true);
        writer_.writeln_unchanged(
            fmt::format("Duplicate in List: {}: {} ({})", context, identifier, user_identifier));
      }
    }

    bool order_changed = false;

    Map<std::string, Item> new_struct_map;
    for (const int64_t i : new_structs.index_range()) {
      const DataWithStruct &new_struct = new_structs[i];
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          new_, new_struct.data, *new_struct.sdna_struct, i);
      const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
          new_, new_struct.data, *new_struct.sdna_struct, i, true);
      if (!new_struct_map.add(identifier, {new_struct, i})) {
        writer_.writeln_unchanged(
            fmt::format("Duplicate in List: {}: {} ({})", context, identifier, user_identifier));
      }
      if (const Item *old_item = old_struct_map.lookup_ptr(identifier)) {
        const DataWithStruct &old_struct = old_item->data_with_struct;
        this->diff_struct(old_block,
                          new_block,
                          intptr_t(old_struct.data) - intptr_t(old_block.data),
                          intptr_t(new_struct.data) - intptr_t(new_block.data),
                          *old_struct.sdna_struct,
                          *new_struct.sdna_struct,
                          fmt::format("{}[{}]", context, user_identifier));
        if (i != old_item->index) {
          order_changed = true;
        }
      }
      else {
        writer_.writeln_added(fmt::format(
            "{}[{}] = {}", context, user_identifier, new_struct.sdna_struct->type->name));
      }
    }

    for (const int64_t i : old_structs.index_range()) {
      const DataWithStruct &old_struct = old_structs[i];
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_struct.data, *old_struct.sdna_struct, i);
      if (new_struct_map.contains(identifier)) {
        continue;
      }
      const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_struct.data, *old_struct.sdna_struct, i, true);
      writer_.writeln_removed(fmt::format(
          "{}[{}] = {}", context, user_identifier, old_struct.sdna_struct->type->name));
    }

    if (old_structs.size() != new_structs.size() || order_changed) {
      writer_.writeln_changed(fmt::format("{} <length> = {}", context, old_structs.size()),
                              fmt::format("{} <length> = {}{}",
                                          context,
                                          new_structs.size(),
                                          order_changed ? " (order changed)" : ""));
    }
  }

  void diff_block_list(const Span<Pointee> old_pointees,
                       const Span<Pointee> new_pointees,
                       const StringRef context)
  {
    struct Item {
      Pointee pointee;
      int64_t index;
    };

    Map<std::string, Item> old_pointee_map;
    for (const int64_t i : old_pointees.index_range()) {
      const Pointee old_pointee = old_pointees[i];
      if (!old_pointee) {
        continue;
      }
      const Struct &old_struct = *old_.sdna.try_find_struct(old_pointee.block->bhead.SDNAnr);
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_pointee.block->data, old_struct, i);
      if (!old_pointee_map.add(identifier, {old_pointee, i})) {
        const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
            old_, old_pointee.block->data, old_struct, i, true);
        writer_.writeln_unchanged(
            fmt::format("Duplicate in List: {}: {} ({})", context, identifier, user_identifier));
      }
    }

    PointeeToStringOptions pointee_to_string_options;
    pointee_to_string_options.include_identifier = false;

    bool order_changed = false;
    Map<std::string, Item> new_pointee_map;
    for (const int64_t i : new_pointees.index_range()) {
      const Pointee new_pointee = new_pointees[i];
      if (!new_pointee) {
        if (old_pointee_map.contains(this->get_index_fallback_identifier(i))) {
          order_changed = true;
        }
        continue;
      }
      const Struct &new_struct = *new_.sdna.try_find_struct(new_pointee.block->bhead.SDNAnr);
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          new_, new_pointee.block->data, new_struct, i);
      const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
          new_, new_pointee.block->data, new_struct, i, true);
      if (!new_pointee_map.add(identifier, {new_pointee, i})) {
        writer_.writeln_unchanged(
            fmt::format("Duplicate in List: {}: {} ({})", context, identifier, user_identifier));
      }
      if (const Item *old_item = old_pointee_map.lookup_ptr(identifier)) {
        if (!new_pointee.id_data && !old_item->pointee.id_data) {
          const BlendBlock &old_pointee = *old_item->pointee.block;
          this->tag_potentially_corresponding_blocks(
              old_pointee, *new_pointee.block, fmt::format("{}[{}]", context, user_identifier));
        }
        if (i != old_item->index) {
          order_changed = true;
        }
      }
      else {
        writer_.writeln_added(
            fmt::format("{}[{}] = {}",
                        context,
                        user_identifier,
                        this->pointee_to_string(new_, new_pointee, pointee_to_string_options)));
      }
    }

    for (const int64_t i : old_pointees.index_range()) {
      const Pointee old_pointee = old_pointees[i];
      if (!old_pointee) {
        continue;
      }
      const Struct &old_struct = *old_.sdna.try_find_struct(old_pointee.block->bhead.SDNAnr);
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_pointee.block->data, old_struct, i);
      if (new_pointee_map.contains(identifier)) {
        continue;
      }
      const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_pointee.block->data, old_struct, i, true);
      writer_.writeln_removed(
          fmt::format("{}[{}] = {}",
                      context,
                      user_identifier,
                      this->pointee_to_string(old_, old_pointee, pointee_to_string_options)));
    }

    if (old_pointees.size() != new_pointees.size() || order_changed) {
      writer_.writeln_changed(fmt::format("{} <length> = {}", context, old_pointees.size()),
                              fmt::format("{} <length> = {}{}",
                                          context,
                                          new_pointees.size(),
                                          order_changed ? " (order changed)" : ""));
    }
  }

  void diff_IDProperty(const BlendBlock &old_block,
                       const BlendBlock &new_block,
                       const int old_struct_offset,
                       const int new_struct_offset,
                       const Struct &old_IDProperty,
                       const Struct &new_IDProperty,
                       const StringRef context)
  {
    const std::optional<char> old_type = try_read_inline_char_member(
        old_block.data + old_struct_offset, old_IDProperty, "type");
    const std::optional<char> new_type = try_read_inline_char_member(
        new_block.data + new_struct_offset, new_IDProperty, "type");
    if (!old_type || !new_type) {
      return;
    }
    if (!ELEM(old_type, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN)) {
      return;
    }
    if (!ELEM(new_type, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN)) {
      return;
    }
    const StructMember *old_data_member = old_IDProperty.members.lookup_key_default_as("data",
                                                                                       nullptr);
    const StructMember *new_data_member = new_IDProperty.members.lookup_key_default_as("data",
                                                                                       nullptr);
    if (!old_data_member || !new_data_member) {
      return;
    }
    if (old_data_member->type->name != "IDPropertyData" ||
        new_data_member->type->name != "IDPropertyData")
    {
      return;
    }
    if (old_data_member->category != StructMember::Category::Struct ||
        new_data_member->category != StructMember::Category::Struct)
    {
      return;
    }
    const Struct &old_IDPropertyData = *old_data_member->type->opt_struct;
    const Struct &new_IDPropertyData = *new_data_member->type->opt_struct;
    const std::optional<int> old_val = try_read_inline_int_member(
        old_block.data + old_struct_offset + old_data_member->offset_in_struct,
        old_IDPropertyData,
        "val");
    const std::optional<int> old_val2 = try_read_inline_int_member(
        old_block.data + old_struct_offset + old_data_member->offset_in_struct,
        old_IDPropertyData,
        "val2");
    const std::optional<int> new_val = try_read_inline_int_member(
        new_block.data + new_struct_offset + new_data_member->offset_in_struct,
        new_IDPropertyData,
        "val");
    const std::optional<int> new_val2 = try_read_inline_int_member(
        new_block.data + new_struct_offset + new_data_member->offset_in_struct,
        new_IDPropertyData,
        "val2");
    if (!old_val || !old_val2 || !new_val || !new_val2) {
      return;
    }
    const PrimitiveValue old_value = this->decode_id_property_value(
        eIDPropertyType(*old_type), *old_val, *old_val2);
    const PrimitiveValue new_value = this->decode_id_property_value(
        eIDPropertyType(*new_type), *new_val, *new_val2);
    if (old_value == new_value) {
      return;
    }
    writer_.writeln_changed(
        fmt::format("{}.decoded_value = {}", context, primitive_value_to_string(old_value)),
        fmt::format("{}.decoded_value = {}", context, primitive_value_to_string(new_value)));
  }

  PrimitiveValue decode_id_property_value(const eIDPropertyType type,
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

  Vector<Pointee> gather_linked_list_pointees(const uint64_t first_address,
                                              const PerBlendData &blend_data)
  {
    Vector<Pointee> pointees;
    uint64_t next_address = first_address;
    while (const Pointee pointee = this->lookup_pointee(blend_data, next_address)) {
      const Struct *sdna_struct = blend_data.sdna.try_find_struct(pointee.block->bhead.SDNAnr);
      if (!sdna_struct) {
        return {};
      }
      if (!this->struct_starts_with_pointer(*sdna_struct)) {
        return {};
      }
      pointees.append(pointee);
      next_address = *reinterpret_cast<const uint64_t *>(pointee.block->data);
    }
    return pointees;
  }

  bool struct_starts_with_pointer(const Struct &sdna_struct) const
  {
    if (sdna_struct.members.is_empty()) {
      return false;
    }
    const StructMember &first_member = *sdna_struct.members[0];
    if (first_member.category == StructMember::Category::Pointer) {
      return true;
    }
    if (first_member.category == StructMember::Category::Struct) {
      return this->struct_starts_with_pointer(*first_member.type->opt_struct);
    }
    return false;
  }

  Pointee lookup_pointee(const PerBlendData &blend_data, const uint64_t address) const
  {
    if (const BlendBlock *block = blend_data.addresses.map.lookup_default(address, nullptr)) {
      return *block;
    }
    if (const BlendIdData *id_data = blend_data.id_addresses.map.lookup_default_as(address,
                                                                                   nullptr))
    {
      return *id_data;
    }
    return {};
  }

  const BlendBlock *lookup_local_data(const PerBlendData &blend_data,
                                      const void *data,
                                      const Struct &sdna_struct,
                                      const StringRef member_name,
                                      const StringRef expected_type) const
  {
    const StructMember *member = sdna_struct.members.lookup_key_default_as(member_name, nullptr);
    if (!member) {
      return nullptr;
    }
    if (!member->is_single_pointer()) {
      return nullptr;
    }
    if (member->type->name != expected_type) {
      return nullptr;
    }
    const uint64_t address = read_address_at_address(
        POINTER_OFFSET(data, member->offset_in_struct));
    return blend_data.addresses.map.lookup_default(address, nullptr);
  }

  struct PointeeToStringOptions {
    bool include_identifier = true;
  };

  std::string pointee_to_string(const PerBlendData &blend_data,
                                const Pointee &pointee,
                                const PointeeToStringOptions &options) const
  {
    if (!pointee) {
      return "nullptr";
    }
    if (const BlendIdData *id_data = pointee.id_data) {
      return fmt::format("{}[\"{}\"]", id_data->type_name, id_data->name);
    }
    const BlendBlock &block = *pointee.block;
    if (block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX) {
      const Span<char> bytes{block.data, block.bhead.len};
      if (const RawBufferType *buffer_type = blend_data.raw_buffer_types.lookup_ptr(&block)) {
        if (buffer_type->pointer_level == 0 && buffer_type->base_type->name == "char" &&
            bytes.size() <= 128)
        {
          if (std::optional<std::string> str = try_convert_char_array_to_readable_string(bytes)) {
            return fmt::format("\"{}\"", *str);
          }
        }
        if (buffer_type->pointer_level == 1) {
          return fmt::format(
              "{}x {}", block.bhead.len / sizeof(void *), buffer_type->base_type->name);
        }
        if (buffer_type->pointer_level == 2) {
          return fmt::format(
              "{}x {} *", block.bhead.len / sizeof(void *), buffer_type->base_type->name);
        }
      }
      const uint64_t hash = XXH3_64bits(bytes.data(), bytes.size());
      return fmt::format("hashed -> 0x{:x}", hash);
    }
    if (const Struct *sdna_struct = blend_data.sdna.try_find_struct(block.bhead.SDNAnr)) {
      const bool is_single = block.bhead.nr == 1;
      const std::string count_str = is_single ? "" : fmt::format("{}x ", block.bhead.nr);
      const std::optional<std::string> user_identifier =
          is_single && options.include_identifier ?
              this->get_struct_identifier(blend_data, block.data, *sdna_struct, true) :
              std::nullopt;
      return fmt::format(
          "{}{}({})", count_str, sdna_struct->type->name, user_identifier.value_or("..."));
    }
    return fmt::format("*");
  }

  void tag_potentially_corresponding_blocks(const BlendBlock &old_block,
                                            const BlendBlock &new_block,
                                            const StringRef context)
  {
    /* A block can only be matched at most once. */
    if (old_by_new_.contains(&new_block)) {
      return;
    }
    if (new_by_old_.contains(&old_block)) {
      return;
    }
    const bool old_is_raw = old_block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX;
    const bool new_is_raw = new_block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX;
    if (old_is_raw != new_is_raw) {
      return;
    }
    /* Blocks with different identifiers cannot be matched. */
    if (this->data_blocks_have_consistent_identifier(old_block, new_block).value_or(true) == false)
    {
      return;
    }
    old_by_new_.add_new(&new_block, &old_block);
    new_by_old_.add_new(&old_block, &new_block);
    matches_to_process_.push({&old_block, &new_block, context});
  }

  std::optional<bool> data_blocks_have_consistent_identifier(const BlendBlock &old_block,
                                                             const BlendBlock &new_block)
  {
    const Struct *old_struct = old_.sdna.try_find_struct(old_block.bhead.SDNAnr);
    const Struct *new_struct = new_.sdna.try_find_struct(new_block.bhead.SDNAnr);
    if (!old_struct || !new_struct) {
      return std::nullopt;
    }
    const std::optional<std::string> old_identifier = this->get_struct_identifier(
        old_, old_block.data, *old_struct);
    const std::optional<std::string> new_identifier = this->get_struct_identifier(
        new_, new_block.data, *new_struct);
    if (old_identifier && new_identifier) {
      return *old_identifier == *new_identifier;
    }
    if (old_identifier.has_value() != new_identifier.has_value()) {
      return false;
    }
    return std::nullopt;
  }

  std::string get_struct_identifier_with_index_fallback(const PerBlendData &blend_data,
                                                        const void *data,
                                                        const Struct &sdna_struct,
                                                        const int64_t index,
                                                        const bool ui_identifier = false) const
  {
    std::optional<std::string> identifier = this->get_struct_identifier(
        blend_data, data, sdna_struct, ui_identifier);
    if (identifier) {
      return std::move(*identifier);
    }
    return this->get_index_fallback_identifier(index);
  }

  std::string get_index_fallback_identifier(const int64_t index) const
  {
    return fmt::format("idx:{}", index);
  }

  std::optional<std::string> get_struct_identifier(const PerBlendData &blend_data,
                                                   const void *data,
                                                   const Struct &sdna_struct,
                                                   const bool ui_identifier = false) const
  {
    if (!sdna_struct.members.is_empty()) {
      const StructMember &first_member = *sdna_struct.members[0];
      if (first_member.category == StructMember::Category::Struct) {
        if (first_member.type->name == "ModifierData") {
          if (ui_identifier) {
            if (std::optional<std::string> name = try_read_inline_string_member(
                    data, *first_member.type->opt_struct, "name"))
            {
              return name;
            }
          }
          else {
            if (const std::optional<int> identifier = try_read_inline_int_member(
                    data, *first_member.type->opt_struct, "persistent_uid"))
            {
              return fmt::format("id:{}", *identifier);
            }
          }
        }
      }
    }
    if (sdna_struct.type->name == "bNode") {
      if (!ui_identifier) {
        if (const std::optional<int> identifier = try_read_inline_int_member(
                data, sdna_struct, "identifier"))
        {
          return fmt::format("id:{}", *identifier);
        }
      }
    }
    if (sdna_struct.type->name == "bNodeSocket") {
      if (!ui_identifier) {
        return try_read_inline_string_member(data, sdna_struct, "identifier");
      }
    }
    if (sdna_struct.type->name == "bNodeLink") {
      return this->get_bNodeLink_identifier(blend_data, data, sdna_struct, ui_identifier);
    }
    if (sdna_struct.type->name == "Attribute") {
      return this->try_read_alloced_string_member(blend_data, data, sdna_struct, "*name");
    }
    if (sdna_struct.type->name == "bNodeTreeInterfacePanel") {
      if (!ui_identifier) {
        if (const std::optional<int> identifier = try_read_inline_int_member(
                data, sdna_struct, "identifier"))
        {
          return fmt::format("id:{}", *identifier);
        }
      }
    }
    if (sdna_struct.type->name == "bNodeTreeInterfaceSocket") {
      if (!ui_identifier) {
        return this->try_read_alloced_string_member(blend_data, data, sdna_struct, "*identifier");
      }
    }
    if (ui_identifier) {
      if (std::optional<std::string> name = try_read_inline_string_member(
              data, sdna_struct, "name"))
      {
        return name;
      }
      if (std::optional<std::string> name = this->try_read_alloced_string_member(
              blend_data, data, sdna_struct, "*name"))
      {
        return name;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> try_read_alloced_string_member(const PerBlendData &blend_data,
                                                            const void *data,
                                                            const Struct &sdna_struct,
                                                            const StringRef member_name) const
  {
    const BlendBlock *name_block = this->lookup_local_data(
        blend_data, data, sdna_struct, member_name, "char");
    if (!name_block) {
      return std::nullopt;
    }
    const Span<char> name_bytes{name_block->data, name_block->bhead.len};
    return try_convert_char_array_to_readable_string(name_bytes);
  }

  std::optional<std::string> get_bNodeLink_identifier(const PerBlendData &blend_data,
                                                      const void *data,
                                                      const Struct &sdna_struct,
                                                      const bool ui_identifier) const
  {
    const BlendBlock *from_node = this->lookup_local_data(
        blend_data, data, sdna_struct, "*fromnode", "bNode");
    const BlendBlock *to_node = this->lookup_local_data(
        blend_data, data, sdna_struct, "*tonode", "bNode");
    const BlendBlock *from_socket = this->lookup_local_data(
        blend_data, data, sdna_struct, "*fromsock", "bNodeSocket");
    const BlendBlock *to_socket = this->lookup_local_data(
        blend_data, data, sdna_struct, "*tosock", "bNodeSocket");
    if (!from_node || !to_node || !from_socket || !to_socket) {
      return std::nullopt;
    }
    const Struct &struct_bNode = *blend_data.sdna.try_find_struct("bNode");
    const Struct &struct_bNodeSocket = *blend_data.sdna.try_find_struct("bNodeSocket");
    const std::optional<std::string> from_node_id = this->get_struct_identifier(
        blend_data, from_node->data, struct_bNode, ui_identifier);
    const std::optional<std::string> to_node_id = this->get_struct_identifier(
        blend_data, to_node->data, struct_bNode, ui_identifier);
    const std::optional<std::string> from_socket_id = this->get_struct_identifier(
        blend_data, from_socket->data, struct_bNodeSocket, ui_identifier);
    const std::optional<std::string> to_socket_id = this->get_struct_identifier(
        blend_data, to_socket->data, struct_bNodeSocket, ui_identifier);
    if (!from_node_id || !to_node_id || !from_socket_id || !to_socket_id) {
      return std::nullopt;
    }
    return fmt::format(
        "{}/{} -> {}/{}", *from_node_id, *from_socket_id, *to_node_id, *to_socket_id);
  }
};

static void write_diff_ids(DiffWriter &writer,
                           const DiffOptions &options,
                           const Span<BlendIdData> id_blocks_old,
                           const Span<BlendIdData> id_blocks_new,
                           const RichSDNA &sdna_old,
                           const RichSDNA &sdna_new)
{
  writer.writeln_unchanged("Data Changes:");
  Map<StringRef, const BlendIdData *> old_id_names;
  Map<StringRef, const BlendIdData *> new_id_names;

  IdAddressMap id_address_map_old;
  IdAddressMap id_address_map_new;

  for (const BlendIdData &id_data : id_blocks_old) {
    old_id_names.add(id_data.name, &id_data);
    id_address_map_old.map.add(uint64_t(id_data.id_block->bhead.old), &id_data);
  }
  for (const BlendIdData &id_data : id_blocks_new) {
    new_id_names.add(id_data.name, &id_data);
    id_address_map_new.map.add(uint64_t(id_data.id_block->bhead.old), &id_data);
  }
  Vector<std::pair<const BlendIdData *, const BlendIdData *>> id_pairs;
  for (const BlendIdData &old_id_data : id_blocks_old) {
    if (const BlendIdData *new_id_data = new_id_names.lookup_default_as(old_id_data.name, nullptr))
    {
      id_pairs.append({&old_id_data, new_id_data});
    }
    else {
      writer.writeln_removed(fmt::format("Data-block: {}", old_id_data.name));
    }
  }
  for (const BlendIdData &new_id_data : id_blocks_new) {
    if (old_id_names.contains(new_id_data.name)) {
      continue;
    }
    writer.writeln_added(fmt::format("Data-block: {}", new_id_data.name));
  }
  for (const std::pair<const BlendIdData *, const BlendIdData *> &id_pair : id_pairs) {
    const BlendIdData &old_id_data = *id_pair.first;
    const BlendIdData &new_id_data = *id_pair.second;
    if (options.ignore_id_type(new_id_data.type_name)) {
      continue;
    }
    IdDiffer id_differ(writer,
                       options,
                       old_id_data,
                       new_id_data,
                       id_address_map_old,
                       id_address_map_new,
                       sdna_old,
                       sdna_new);
    id_differ.run();
  }
}

static bool is_specific_id_struct(const Struct &sdna_struct)
{
  if (sdna_struct.members.is_empty()) {
    return false;
  }
  const StructMember &first_member = *sdna_struct.members[0];
  if (first_member.identifier != "id") {
    return false;
  }
  if (first_member.type->name != "ID") {
    return false;
  }
  return true;
}

static bool is_id_block(const BlendBlock &block, const RichSDNA &sdna)
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
      const Struct *sdna_struct = sdna.try_find_struct(block.bhead.SDNAnr);
      if (!sdna_struct) {
        return false;
      }
      if (!is_specific_id_struct(*sdna_struct)) {
        return false;
      }
      return true;
    }
  }
}

static bool sdna_fullfills_core_assumptions(const RichSDNA &sdna)
{
  const Struct *id_struct = sdna.try_find_struct("ID");
  if (!id_struct) {
    return false;
  }
  const StructMember *id_name_member = id_struct->members.lookup_key_default_as("name", nullptr);
  if (!id_name_member) {
    return false;
  }
  if (!id_name_member->is_char_array()) {
    return false;
  }
  for (const Struct *sdna_struct : sdna.structs) {
    for (const StructMember *sdna_member : sdna_struct->members) {
      if (sdna_member->size_in_bytes <= 0) {
        return false;
      }
      if (sdna_member->elem_num <= 0) {
        return false;
      }
    }
  }
  const Struct *listbase_struct = sdna.try_find_struct("ListBase");
  if (!listbase_struct) {
    return false;
  }
  if (listbase_struct->members.size() != 2) {
    return false;
  }
  if (!listbase_struct->members[0]->is_single_pointer()) {
    return false;
  }
  if (!listbase_struct->members[1]->is_single_pointer()) {
    return false;
  }
  return true;
}

static bool block_sizes_match_sdna(const BlendData &blend_data, const RichSDNA &sdna)
{
  for (const BlendBlock &block : blend_data.blocks) {
    switch (block.bhead.code) {
      case BLO_CODE_DATA: {
        if (block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX) {
          continue;
        }
        const Struct *sdna_struct = sdna.try_find_struct(block.bhead.SDNAnr);
        if (!sdna_struct) {
          return false;
        }
        const int64_t expected_size = sdna_struct->type->size_in_bytes * block.bhead.nr;
        const int64_t actual_size = block.bhead.len;
        if (expected_size != actual_size) {
          return false;
        }
        break;
      }
    }
  }
  return true;
}

static std::optional<Vector<BlendIdData>> find_blend_id_blocks(const BlendData &blend_data,
                                                               const RichSDNA &sdna)
{
  Vector<BlendIdData> result;

  const Struct &id_sdna_struct = *sdna.try_find_struct("ID");

  int64_t i = 0;
  while (i < blend_data.blocks.size()) {
    const BlendBlock &block = blend_data.blocks[i];
    if (!is_id_block(block, sdna)) {
      i++;
      continue;
    }
    const std::optional<std::string> name = try_read_inline_string_member(
        block.data, id_sdna_struct, "name");
    if (!name) {
      return std::nullopt;
    }
    if (name->size() <= 2) {
      return std::nullopt;
    }
    const Struct *id_struct = sdna.try_find_struct(block.bhead.SDNAnr);
    if (!id_struct) {
      return std::nullopt;
    }

    BlendIdData id_data;
    id_data.name = *name;
    id_data.id_block = &block;
    id_data.type_name = id_struct->type->name;
    i++;
    const int first_data_index = i;
    while (i < blend_data.blocks.size()) {
      const BlendBlock &next_block = blend_data.blocks[i];
      if (next_block.bhead.code != BLO_CODE_DATA) {
        break;
      }
      i++;
    }
    id_data.blocks = blend_data.blocks.as_span().slice(
        IndexRange::from_begin_end(first_data_index, i));
    result.append(std::move(id_data));
  }

  return result;
}

static SDNA *parse_raw_sdna(const BlendBlock &block)
{
  BLI_assert(block.bhead.code == BLO_CODE_DNA1);
  return DNA_sdna_from_data(block.data, block.bhead.len, false, true, nullptr);
}

static std::optional<BlendData> read_blend_file_data(FileReader &file)
{
  const BlenderHeaderVariant header_variant = BLO_readfile_blender_header_decode(&file);
  const BlenderHeader *header = std::get_if<BlenderHeader>(&header_variant);
  if (!header) {
    return std::nullopt;
  }
  const BHeadType bhead_type = header->bhead_type();
  BlendData blend_file_data;
  blend_file_data.allocator = std::make_unique<LinearAllocator<>>();
  blend_file_data.header = *header;
  while (const std::optional<BHead> bhead = BLO_readfile_read_bhead(&file, bhead_type)) {
    if (bhead->len < 0) {
      return std::nullopt;
    }
    void *data = blend_file_data.allocator->allocate(bhead->len, 16);
    const int64_t read_size = file.read(&file, data, bhead->len);
    if (read_size != bhead->len) {
      return std::nullopt;
    }
    const int index = blend_file_data.blocks.append_and_get_index(
        BlendBlock{*bhead, static_cast<const char *>(data)});
    switch (bhead->code) {
      case BLO_CODE_DNA1: {
        blend_file_data.sdna_block_index = index;
        break;
      }
      case BLO_CODE_GLOB: {
        blend_file_data.global_block_index = index;
        break;
      }
    }
  }
  if (blend_file_data.sdna_block_index == -1) {
    return std::nullopt;
  }
  if (blend_file_data.global_block_index == -1) {
    return std::nullopt;
  }
  return blend_file_data;
}

static void handle_invalid_blend_file_error(const StringRef path)
{
  std::fstream f(path, std::ios::in | std::ios::binary);
  if (!f.is_open()) {
    fmt::println(stderr, "Unable to open file: {}", path);
    return;
  }
  char first_bytes[50] = {};
  if (f.read(first_bytes, sizeof(first_bytes) - 1)) {
    const char *lfs_magic = "version https://git-lfs";
    if (memcmp(first_bytes, lfs_magic, strlen(lfs_magic)) == 0) {
      fmt::println(stderr, "File is a git lfs file: {}", path);
      return;
    }
  }

  fmt::println(stderr, "Unable to read .blend file: {}", path);
}

static int main_do(const int argc, char *argv[])
{
  // std::fstream myfile("/home/jacques/Downloads/test.txt", std::ios::out);
  // for (const int i : blender::IndexRange(argc)) {
  //   myfile << argv[i] << '\n';
  // }
  // myfile.close();
  // std::this_thread::sleep_for(std::chrono::seconds(10));
  if (argc < 3) {
    fmt::println(stderr, "Incorrect usage");
    return 1;
  }
  const StringRefNull relative_path = argv[1];
  const StringRefNull file_old = argv[2];
  const StringRefNull file_new = argv[5];

  FileReader *file_reader_old = BLO_file_reader_uncompressed_from_path(file_old.c_str());
  FileReader *file_reader_new = BLO_file_reader_uncompressed_from_path(file_new.c_str());
  BLI_SCOPED_DEFER([&]() {
    if (file_reader_old) {
      file_reader_old->close(file_reader_old);
    }
    if (file_reader_new) {
      file_reader_new->close(file_reader_new);
    }
  });
  if (!file_reader_old) {
    handle_invalid_blend_file_error(file_old);
    return 1;
  }
  if (!file_reader_new) {
    handle_invalid_blend_file_error(file_new);
    return 1;
  }

  const std::optional<BlendData> blend_data_old = read_blend_file_data(*file_reader_old);
  const std::optional<BlendData> blend_data_new = read_blend_file_data(*file_reader_new);
  if (!blend_data_old) {
    fmt::println(stderr, "Unable to read .blend file: {}", file_old);
    return 1;
  }
  if (!blend_data_new) {
    fmt::println(stderr, "Unable to read .blend file: {}", file_new);
    return 1;
  }
  if (blend_data_old->header.pointer_size != 8 || blend_data_new->header.pointer_size != 8) {
    fmt::println(stderr, "Only .blend files with 64 bit pointers are supported");
    return 1;
  }

  SDNA *raw_sdna_old = parse_raw_sdna(blend_data_old->blocks[blend_data_old->sdna_block_index]);
  SDNA *raw_sdna_new = parse_raw_sdna(blend_data_new->blocks[blend_data_new->sdna_block_index]);
  if (!raw_sdna_old) {
    fmt::println(stderr, "Unable to parse SDNA: {}", file_old);
    return 1;
  }
  if (!raw_sdna_new) {
    fmt::println(stderr, "Unable to parse SDNA: {}", file_new);
    return 1;
  }
  BLI_SCOPED_DEFER([&]() { DNA_sdna_free(raw_sdna_old); });
  BLI_SCOPED_DEFER([&]() { DNA_sdna_free(raw_sdna_new); });

  using namespace rich_sdna;
  const RichSDNA sdna_old{*raw_sdna_old};
  const RichSDNA sdna_new{*raw_sdna_new};

  if (!sdna_fullfills_core_assumptions(sdna_old)) {
    fmt::println(stderr, "SDNA does not fullfill core assumptions");
    return 1;
  }
  if (!sdna_fullfills_core_assumptions(sdna_new)) {
    fmt::println(stderr, "SDNA does not fullfill core assumptions");
    return 1;
  }

  if (!block_sizes_match_sdna(*blend_data_old, sdna_old)) {
    fmt::println(stderr, "Block sizes do not match SDNA");
    return 1;
  }
  if (!block_sizes_match_sdna(*blend_data_new, sdna_new)) {
    fmt::println(stderr, "Block sizes do not match SDNA");
    return 1;
  }

  const std::optional<Vector<BlendIdData>> id_blocks_old = find_blend_id_blocks(*blend_data_old,
                                                                                sdna_old);
  const std::optional<Vector<BlendIdData>> id_blocks_new = find_blend_id_blocks(*blend_data_new,
                                                                                sdna_new);

  if (!id_blocks_old) {
    fmt::println(stderr, "Unable to find ID blocks in old SDNA");
    return 1;
  }
  if (!id_blocks_new) {
    fmt::println(stderr, "Unable to find ID blocks in new SDNA");
    return 1;
  }

  DiffOptions options;
  options.ignore_pad = true;
  options.add_members_to_ignore(
      "bNode", {"locx", "locy", "width", "height", "ui_order", "location", "type"});
  options.add_members_to_ignore("bNodeTree", {"view_center"});
  options.add_members_to_ignore("bNodeSocket", {"*link"});
  options.add_members_to_ignore("ID", {"session_uid", "recalc_up_to_undo_push"});
  options.add_members_to_ignore("CustomData", {"typemap"});
  options.add_members_to_ignore("bNodeTreeInterface", {"active_index"});
  options.add_members_to_ignore("IDProperty", {"totallen"});
  options.add_members_to_ignore("CurveProfile", {"changed_timestamp"});
  options.add_members_to_ignore("bNodeLink", {"*fromnode", "*tonode", "*fromsock", "*tosock"});
  options.add_next_prev_ignore_types(
      {"bNode", "bNodeSocket", "bNodeLink", "IDProperty", "ModifierData"});
  options.add_ignored_flags("bNode", "flag", NODE_SELECT | NODE_OPTIONS | NODE_ACTIVE);
  options.add_ignored_flags(
      "bNodeSocket", "flag", SELECT | SOCK_HIDDEN | SOCK_IS_LINKED | SOCK_COLLAPSED);
  options.add_id_types_to_ignore({"wmWindowManager", "Screen", "WorkSpace"});
  /* These have special handling. */
  options.add_members_to_ignore("IDPropertyData", {"val", "val2"});

  DiffWriter writer(relative_path);
  write_diff_sdna(writer, options, sdna_old, sdna_new);
  write_diff_ids(writer, options, *id_blocks_old, *id_blocks_new, sdna_old, sdna_new);

  std::cout << writer.to_string();
  return 0;
}

}  // namespace blender::blend_diff

int main(int argc, char *argv[])
{
  try {
    return blender::blend_diff::main_do(argc, argv);
  }
  catch (const std::exception &e) {
    fmt::println(stderr, "Exception: {}", e.what());
    return 1;
  }
}
