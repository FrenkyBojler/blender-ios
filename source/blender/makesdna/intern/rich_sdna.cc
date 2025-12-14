/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_genfile.h"
#include "DNA_rich_sdna.hh"

#include "BLI_timeit.hh"

// #include "BLI_strict_flags.h"

namespace blender::rich_sdna {

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

struct ParsedSdnaBuffer {
  Vector<StringRefNull> member_names;
  Vector<StringRefNull> type_names;
  Vector<int> type_sizes;

  struct MemberItem {
    int type_i;
    int name_i;
  };

  struct StructItem {
    int type_i;
    Vector<MemberItem> members;
  };

  Vector<StructItem> structs;
};

class SdnaParser {
 private:
  Span<char> buffer_;
  int64_t i_ = 0;

 public:
  SdnaParser(Span<char> buffer) : buffer_(buffer) {}

  std::optional<ParsedSdnaBuffer> parse()
  {
    ParsedSdnaBuffer result;
    if (!this->consume_magic("SDNA")) {
      return std::nullopt;
    }
    if (!this->parse_member_names(result)) {
      return std::nullopt;
    }
    if (!this->parse_type_names(result)) {
      return std::nullopt;
    }
    if (!this->parse_type_sizes(result)) {
      return std::nullopt;
    }
    if (!this->parse_structs(result)) {
      return std::nullopt;
    }
    if (i_ != buffer_.size()) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] bool parse_member_names(ParsedSdnaBuffer &result)
  {
    if (!this->consume_magic("NAME")) {
      return false;
    }
    std::optional<Vector<StringRefNull>> names = this->consume_name_list();
    if (!names) {
      return false;
    }
    result.member_names = std::move(*names);
    return true;
  }

  [[nodiscard]] bool parse_type_names(ParsedSdnaBuffer &result)
  {
    if (!this->consume_padding_to_4_bytes()) {
      return false;
    }
    if (!this->consume_magic("TYPE")) {
      return false;
    }
    std::optional<Vector<StringRefNull>> names = this->consume_name_list();
    if (!names) {
      return false;
    }
    result.type_names = std::move(*names);
    return true;
  }

  [[nodiscard]] bool parse_type_sizes(ParsedSdnaBuffer &result)
  {
    if (!this->consume_padding_to_4_bytes()) {
      return false;
    }
    if (!this->consume_magic("TLEN")) {
      return false;
    }
    result.type_sizes.resize(result.type_names.size());
    for (const int64_t i : result.type_names.index_range()) {
      const std::optional<int16_t> type_size = this->consume_int16();
      if (!type_size) {
        return false;
      }
      if (*type_size < 0) {
        return false;
      }
      result.type_sizes[i] = *type_size;
    }
    return true;
  }

  [[nodiscard]] bool parse_structs(ParsedSdnaBuffer &result)
  {
    if (!this->consume_padding_to_4_bytes()) {
      return false;
    }
    if (!this->consume_magic("STRC")) {
      return false;
    }
    const std::optional<int> structs_num = this->consume_int32();
    if (!structs_num) {
      return false;
    }
    result.structs.resize(*structs_num);
    for (const int64_t struct_i : IndexRange(*structs_num)) {
      const std::optional<int16_t> struct_type_i = this->consume_int16();
      if (!struct_type_i) {
        return false;
      }
      const std::optional<int16_t> members_num = this->consume_int16();
      if (!members_num) {
        return false;
      }
      if (*members_num < 0) {
        return false;
      }
      result.structs[struct_i].type_i = *struct_type_i;
      result.structs[struct_i].members.resize(*members_num);
      for (const int64_t member_i : IndexRange(*members_num)) {
        const std::optional<int16_t> member_type_i = this->consume_int16();
        if (!member_type_i) {
          return false;
        }
        const std::optional<int16_t> member_name_i = this->consume_int16();
        if (!member_name_i) {
          return false;
        }
        result.structs[struct_i].members[member_i].type_i = *member_type_i;
        result.structs[struct_i].members[member_i].name_i = *member_name_i;
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<Vector<StringRefNull>> consume_name_list()
  {
    Vector<StringRefNull> names;
    const std::optional<int> names_num = this->consume_int32();
    if (!names_num) {
      return std::nullopt;
    }
    if (*names_num < 0) {
      return std::nullopt;
    }
    names.resize(*names_num);
    for (const int64_t i : IndexRange(*names_num)) {
      const std::optional<StringRefNull> name = this->consume_c_string();
      if (!name) {
        return std::nullopt;
      }
      names[i] = *name;
    }
    return names;
  }

  [[nodiscard]] bool consume_magic(const StringRef magic)
  {
    return this->consume_magic(Span<char>(magic.data(), magic.size()));
  }

  [[nodiscard]] bool consume_magic(const Span<char> magic)
  {
    const Span<char> slice = buffer_.slice_safe(i_, magic.size());
    if (slice != magic) {
      return false;
    }
    i_ += magic.size();
    return true;
  }

  [[nodiscard]] bool consume_padding_to_4_bytes()
  {
    if (i_ % 4 == 0) {
      return true;
    }
    const int64_t padding = 4 - (i_ % 4);
    if (buffer_.size() - i_ < padding) {
      return false;
    }
    i_ += padding;
    return true;
  }

  [[nodiscard]] std::optional<int> consume_int32()
  {
    if (buffer_.size() - i_ < int64_t(sizeof(int))) {
      return std::nullopt;
    }
    const int value = *reinterpret_cast<const int *>(buffer_.data() + i_);
    i_ += int64_t(sizeof(int));
    return value;
  }

  [[nodiscard]] std::optional<int16_t> consume_int16()
  {
    if (buffer_.size() - i_ < int64_t(sizeof(int16_t))) {
      return std::nullopt;
    }
    const int16_t value = *reinterpret_cast<const int16_t *>(buffer_.data() + i_);
    i_ += int64_t(sizeof(int16_t));
    return value;
  }

  [[nodiscard]] std::optional<StringRefNull> consume_c_string()
  {
    const int64_t start = i_;
    while (true) {
      if (i_ >= buffer_.size()) {
        return std::nullopt;
      }
      const char c = buffer_[i_];
      if (c == '\0') {
        const StringRefNull str = StringRefNull(buffer_.data() + start, i_ - start);
        i_++;
        return str;
      }
      i_++;
    }
  }
};

std::optional<ParsedSdnaBuffer> parse_sdna_buffer(const void *buffer, const int64_t buffer_size)
{
  SCOPED_TIMER(__func__);
  SdnaParser parser{Span<char>(static_cast<const char *>(buffer), buffer_size)};
  return parser.parse();
}

struct PrimitiveTypeInfo {
  PrimitiveType type;
  int expected_size;
};

static const Map<StringRef, PrimitiveTypeInfo> &get_primitive_type_map()
{
  static Map<StringRef, PrimitiveTypeInfo> map = []() {
    Map<StringRef, PrimitiveTypeInfo> map;
    map.add("char", {PrimitiveType::Char, 1});
    map.add("uchar", {PrimitiveType::UChar, 1});
    map.add("short", {PrimitiveType::Short, 2});
    map.add("ushort", {PrimitiveType::UShort, 2});
    map.add("int", {PrimitiveType::Int, 4});
    map.add("float", {PrimitiveType::Float, 4});
    map.add("double", {PrimitiveType::Double, 8});
    map.add("int64_t", {PrimitiveType::Int64, 8});
    map.add("uint64_t", {PrimitiveType::UInt64, 8});
    map.add("int8_t", {PrimitiveType::Int8, 1});
    return map;
  }();
  return map;
}

std::unique_ptr<RichSDNA> RichSDNA::from_sdna_buffer(const void *buffer, const int64_t buffer_size)
{
  std::optional<ParsedSdnaBuffer> parsed = parse_sdna_buffer(buffer, buffer_size);

  SDNA *raw_sdna = DNA_sdna_from_data(buffer, buffer_size, false, true, nullptr);
  if (!raw_sdna) {
    return nullptr;
  }
  std::unique_ptr<RichSDNA> rich_sdna = RichSDNA::from_sdna(*raw_sdna);
  DNA_sdna_free(raw_sdna);
  return rich_sdna;
}

std::unique_ptr<RichSDNA> RichSDNA::from_sdna(const SDNA &raw_sdna)
{
  auto rich_sdna = std::make_unique<RichSDNA>();

  const Map<StringRef, PrimitiveTypeInfo> &primitive_type_map = get_primitive_type_map();

  LinearAllocator<> &allocator = rich_sdna->scope_.allocator();
  for (const int type_i : IndexRange(raw_sdna.types_num)) {
    const StringRefNull type_name = allocator.copy_string(raw_sdna.types[type_i]);
    Type &sdna_type = rich_sdna->scope_.construct<Type>();
    sdna_type.owner = &*rich_sdna;
    sdna_type.name = type_name;
    sdna_type.size_in_bytes = raw_sdna.types_size[type_i];
    sdna_type.index = type_i;

    if (const PrimitiveTypeInfo *primitive_type_info = primitive_type_map.lookup_ptr(type_name)) {
      if (sdna_type.size_in_bytes != primitive_type_info->expected_size) {
        return nullptr;
      }
      sdna_type.opt_primitive_type = primitive_type_info->type;
    }
    rich_sdna->types.add(&sdna_type);
  }
  for (const int struct_i : IndexRange(raw_sdna.structs_num)) {
    const SDNA_Struct &raw_struct = *raw_sdna.structs[struct_i];
    Struct &sdna_struct = rich_sdna->scope_.construct<Struct>();
    sdna_struct.type = rich_sdna->types[raw_struct.type_index];
    const_cast<Type *>(sdna_struct.type)->opt_struct = &sdna_struct;

    for (const int member_i : IndexRange(raw_struct.members_num)) {
      const SDNA_StructMember &raw_member = raw_struct.members[member_i];
      StructMember &sdna_member = rich_sdna->scope_.construct<StructMember>();
      sdna_member.elem_num = raw_sdna.members_array_num[raw_member.member_index];
      sdna_member.type = rich_sdna->types[raw_member.type_index];
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
    rich_sdna->structs.add(&sdna_struct);
  }
  for (const Struct *sdna_struct_const : rich_sdna->structs) {
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

  return rich_sdna;
}

void RichSDNA::print(std::ostream &stream, const bool verbose) const
{
  for (const int type_i : this->types.index_range()) {
    const Type &type = *this->types[type_i];
    type.print(stream, verbose);
  }
}

const Struct *RichSDNA::try_find_struct(const int struct_nr) const
{
  if (struct_nr < 0 || struct_nr >= this->structs.size()) {
    return nullptr;
  }
  return this->structs[struct_nr];
}

const Struct *RichSDNA::try_find_struct(const StringRef name) const
{
  return this->structs.lookup_key_default_as(name, nullptr);
}

const Type *RichSDNA::try_find_type(const StringRef name) const
{
  return this->types.lookup_key_default_as(name, nullptr);
}

void Type::print(std::ostream &stream, const bool verbose) const
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

std::string StructMember::to_decl_string() const
{
  return fmt::format(
      "{} {}::{}", this->type->name, this->parent->type->name, this->name_with_array);
}

}  // namespace blender::rich_sdna
