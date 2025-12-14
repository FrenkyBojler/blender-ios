/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_rich_sdna.hh"

#include "dna_utils.h"

#include "BLI_strict_flags.h" /* IWYU pragma: keep. Keep last. */

namespace blender::rich_sdna {

static bool name_is_pointer(const StringRefNull name)
{
  return name[0] == '*' || (name[0] == '(' && name[1] == '*');
}

static std::string strip_name(const StringRef identifier)
{
  std::string result;
  for (const char c : identifier) {
    if (c == '[') {
      break;
    }
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

static std::optional<ParsedSdnaBuffer> parse_sdna_buffer(const void *buffer,
                                                         const int64_t buffer_size)
{
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
    Map<StringRef, PrimitiveTypeInfo> m;
    m.add("char", {PrimitiveType::Char, 1});
    m.add("uchar", {PrimitiveType::UChar, 1});
    m.add("short", {PrimitiveType::Short, 2});
    m.add("ushort", {PrimitiveType::UShort, 2});
    m.add("int", {PrimitiveType::Int, 4});
    m.add("float", {PrimitiveType::Float, 4});
    m.add("double", {PrimitiveType::Double, 8});
    m.add("int64_t", {PrimitiveType::Int64, 8});
    m.add("uint64_t", {PrimitiveType::UInt64, 8});
    m.add("int8_t", {PrimitiveType::Int8, 1});
    return m;
  }();
  return map;
}

std::unique_ptr<RichSDNA> RichSDNA::from_sdna_buffer(const void *buffer, const int64_t buffer_size)
{
  const std::optional<ParsedSdnaBuffer> parsed = parse_sdna_buffer(buffer, buffer_size);
  if (!parsed) {
    return nullptr;
  }
  if (parsed->type_names.size() != parsed->type_sizes.size()) {
    return nullptr;
  }
  auto rich_sdna = std::make_unique<RichSDNA>();
  rich_sdna->pointer_size = int64_t(sizeof(void *));
  const Map<StringRef, PrimitiveTypeInfo> &primitive_type_map = get_primitive_type_map();
  ResourceScope &scope = rich_sdna->scope_;
  LinearAllocator<> &allocator = scope.allocator();

  auto is_valid_type_i = [&](const int64_t type_i) {
    return parsed->type_names.index_range().contains(type_i);
  };
  auto is_valid_member_name_i = [&](const int64_t name_i) {
    return parsed->member_names.index_range().contains(name_i);
  };

  for (const int64_t type_i : parsed->type_names.index_range()) {
    const StringRefNull type_name = allocator.copy_string(parsed->type_names[type_i]);
    Type &sdna_type = scope.construct<Type>();
    sdna_type.owner = &*rich_sdna;
    sdna_type.index = type_i;
    sdna_type.name = type_name;
    sdna_type.size_in_bytes = parsed->type_sizes[type_i];
    if (sdna_type.size_in_bytes < 0) {
      return nullptr;
    }
    if (const PrimitiveTypeInfo *primitive_type_info = primitive_type_map.lookup_ptr(type_name)) {
      if (sdna_type.size_in_bytes != primitive_type_info->expected_size) {
        return nullptr;
      }
      sdna_type.opt_primitive_type = primitive_type_info->type;
    }
    if (!rich_sdna->types.add(&sdna_type)) {
      return nullptr;
    }
  }
  for (const int64_t struct_i : parsed->structs.index_range()) {
    const ParsedSdnaBuffer::StructItem &raw_struct = parsed->structs[struct_i];
    if (!is_valid_type_i(raw_struct.type_i)) {
      return nullptr;
    }
    Struct &sdna_struct = scope.construct<Struct>();
    Type &type = const_cast<Type &>(*rich_sdna->types[raw_struct.type_i]);
    sdna_struct.type = &type;
    type.opt_struct = &sdna_struct;
    for (const int64_t member_i : raw_struct.members.index_range()) {
      const ParsedSdnaBuffer::MemberItem &raw_member = raw_struct.members[member_i];
      if (!is_valid_type_i(raw_member.type_i)) {
        return nullptr;
      }
      if (!is_valid_member_name_i(raw_member.name_i)) {
        return nullptr;
      }
      const StringRefNull raw_member_name = allocator.copy_string(
          parsed->member_names[raw_member.name_i]);
      StructMember &sdna_member = scope.construct<StructMember>();
      const int elem_num = DNA_member_array_num(raw_member_name.c_str());
      if (elem_num <= 0 || elem_num > 64 * 1024) {
        return nullptr;
      }
      sdna_member.elem_num = elem_num;
      sdna_member.type = rich_sdna->types[raw_member.type_i];
      sdna_member.raw_name = raw_member_name;
      sdna_member.name = allocator.copy_string(strip_name(raw_member_name.c_str()));
      sdna_member.parent = &sdna_struct;
      if (!sdna_struct.members.add(&sdna_member)) {
        return nullptr;
      }
    }
    if (!rich_sdna->structs.add(&sdna_struct)) {
      return nullptr;
    }
  }
  for (const Struct *sdna_struct_const : rich_sdna->structs) {
    Struct &sdna_struct = const_cast<Struct &>(*sdna_struct_const);
    int64_t offset = 0;
    for (const StructMember *sdna_member_const : sdna_struct.members) {
      StructMember &sdna_member = const_cast<StructMember &>(*sdna_member_const);
      sdna_member.offset_in_struct = offset;
      if (name_is_pointer(sdna_member.raw_name)) {
        sdna_member.category = StructMember::Category::Pointer;
        sdna_member.elem_size = rich_sdna->pointer_size;
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
    if (offset != sdna_struct.type->size_in_bytes) {
      return nullptr;
    }
  }

  return rich_sdna;
}

void RichSDNA::print(std::ostream &stream, const bool verbose) const
{
  for (const int64_t type_i : this->types.index_range()) {
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
    for (const int64_t member_i : this->opt_struct->members.index_range()) {
      const StructMember &member = *this->opt_struct->members[member_i];
      if (verbose) {
        fmt::format_to(dst, "    {}\n", member.name);
        fmt::format_to(dst, "      Raw Name: {}\n", member.raw_name);
        fmt::format_to(dst, "      Offset in struct: {}\n", member.offset_in_struct);
        fmt::format_to(dst, "      Elem size: {}\n", member.elem_size);
        fmt::format_to(dst, "      Elem num: {}\n", member.elem_num);
        fmt::format_to(dst, "      Size in bytes: {}\n", member.size_in_bytes);
        fmt::format_to(dst, "      Category: {}\n", int(member.category));
        fmt::format_to(dst, "      Type: {}\n", member.type->name);
      }
      else {
        fmt::format_to(dst, "    {} {}\n", member.type->name, member.raw_name);
      }
    }
  }
  stream << fmt::to_string(mem_buf);
}

std::string StructMember::to_decl_string() const
{
  return fmt::format("{} {}::{}", this->type->name, this->parent->type->name, this->raw_name);
}

}  // namespace blender::rich_sdna
