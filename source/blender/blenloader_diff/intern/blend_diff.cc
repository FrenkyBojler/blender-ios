/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <toml.hpp>
#include <xxhash.h>

#include "BLI_generic_span.hh"
#include "BLI_index_range.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_path_utils.hh"
#include "BLI_resource_scope.hh"
#include "BLI_set.hh"
#include "BLI_stack.hh"
#include "BLI_string.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_node_types.h"
#include "DNA_rich_sdna.hh"
#include "DNA_sdna_types.h"

#include "blend_query.hh"

namespace blender::blend_diff {

using rich_sdna::PrimitiveType;
using rich_sdna::PrimitiveValue;
using rich_sdna::RichSDNA;
using rich_sdna::Struct;
using rich_sdna::StructMember;
using rich_sdna::Type;

using blend_query::BlendBlock;
using blend_query::BlendId;
using blend_query::BlendQuery;
using blend_query::BlendValue;
using blend_query::MemType;

struct DiffOptions {
  ResourceScope scope_;
  bool ignore_pad = true;
  bool ignore_runtime = true;
  int64_t max_array_changes = 16;

  struct MemberName {
    StringRef type_name;
    StringRef member_identifier;

    MemberName(const StringRef type_name, const StringRef member_name)
        : type_name(type_name), member_identifier(member_name)
    {
    }

    MemberName(const StructMember &member)
        : type_name(member.parent->type->name), member_identifier(member.name)
    {
    }

    uint64_t hash() const
    {
      return get_default_hash(this->type_name, this->member_identifier);
    }

    bool operator==(const MemberName &other) const = default;
  };

  Set<MemberName> members_to_ignore_set;
  Set<std::string> id_types_to_ignore;
  Map<MemberName, uint64_t> ignored_flags;
  Set<MemberName> dont_follow_members;

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
      this->add_members_to_ignore(type_name, {"next", "prev"});
    }
  }

  void add_id_types_to_ignore(const Span<StringRef> type_names)
  {
    for (const StringRef type_name : type_names) {
      this->id_types_to_ignore.add(scope_.allocator().copy_string(type_name));
    }
  }

  void add_ignored_flags(const StringRef type_name,
                         const StringRef member_name,
                         const uint64_t flag)
  {
    LinearAllocator<> &allocator = scope_.allocator();
    this->ignored_flags.add({allocator.copy_string(type_name), allocator.copy_string(member_name)},
                            flag);
  }

  bool ignore_member(const StructMember &member) const
  {
    if (this->ignore_pad) {
      if (member.name.startswith("_pad")) {
        return true;
      }
    }
    if (this->ignore_runtime) {
      if (member.name.find("runtime") != StringRef::not_found) {
        return true;
      }
    }
    if (member.name == "prev" || member.name == "next") {
      return true;
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

static bool is_specific_id_struct(const Struct &sdna_struct)
{
  if (sdna_struct.members.is_empty()) {
    return false;
  }
  const StructMember &first_member = *sdna_struct.members[0];
  if (first_member.name != "id") {
    return false;
  }
  if (first_member.type->name != "ID") {
    return false;
  }
  return true;
}

class DiffLines {
 private:
  std::unique_ptr<fmt::memory_buffer> mem_buf_;
  bool is_empty_ = true;

 public:
  DiffLines()
  {
    mem_buf_ = std::make_unique<fmt::memory_buffer>();
  }

  void change(const StringRef old_line, const StringRef new_line, bool skip_if_same = true)
  {
    if (skip_if_same) {
      if (old_line == new_line) {
        return;
      }
    }
    this->remove(old_line);
    this->add(new_line);
  }

  void add(const StringRef line)
  {
    fmt::format_to(fmt::appender(*mem_buf_), "+{}\n", line);
    is_empty_ = false;
  }

  void remove(const StringRef line)
  {
    fmt::format_to(fmt::appender(*mem_buf_), "-{}\n", line);
    is_empty_ = false;
  }

  void info(const StringRef line)
  {
    fmt::format_to(fmt::appender(*mem_buf_), " {}\n", line);
    is_empty_ = false;
  }

  bool is_empty() const
  {
    return is_empty_;
  }

  std::string to_string() const
  {
    return fmt::to_string(*mem_buf_);
  }
};

static void write_diff_struct_member(DiffLines &diff,
                                     const StructMember &old_member,
                                     const StructMember &new_member)
{
  const std::string old_member_str = old_member.to_decl_string();
  const std::string new_member_str = new_member.to_decl_string();
  if (old_member_str == new_member_str) {
    return;
  }
  diff.change(old_member_str, new_member_str);
}

static void write_diff_type(DiffLines &diff,
                            const DiffOptions &options,
                            const Type &old_type,
                            const Type &new_type)
{
  const StringRef name = old_type.name;
  if (old_type.opt_struct && !new_type.opt_struct) {
    diff.change(fmt::format("Type `{}` has struct", name),
                fmt::format("Type `{}` has no struct", name));
    return;
  }
  if (!old_type.opt_struct && new_type.opt_struct) {
    diff.change(fmt::format("Type `{}` has no struct", name),
                fmt::format("Type `{}` has struct", name));
    return;
  }
  if (!old_type.opt_struct) {
    if (old_type.size_in_bytes != new_type.size_in_bytes) {
      diff.change(fmt::format("Type `{}` has size {}", name, old_type.size_in_bytes),
                  fmt::format("Type `{}` has size {}", name, new_type.size_in_bytes));
    }
    return;
  }
  for (const StructMember *old_member : old_type.opt_struct->members) {
    if (options.ignore_member(*old_member)) {
      continue;
    }
    if (const StructMember *new_member = new_type.opt_struct->members.lookup_key_default_as(
            old_member->name, nullptr))
    {
      write_diff_struct_member(diff, *old_member, *new_member);
    }
    else {
      diff.remove(old_member->to_decl_string());
    }
  }
  for (const StructMember *new_member : new_type.opt_struct->members) {
    if (options.ignore_member(*new_member)) {
      continue;
    }
    const StructMember *old_member = old_type.opt_struct->members.lookup_key_default_as(
        new_member->name, nullptr);
    if (old_member) {
      continue;
    }
    diff.add(new_member->to_decl_string());
  }
}

static DiffLines write_diff_sdna(const DiffOptions &options,
                                 const RichSDNA &old_sdna,
                                 const RichSDNA &new_sdna)
{
  DiffLines diff;
  for (const Type *old_type : old_sdna.types) {
    if (const Type *new_type = new_sdna.types.lookup_key_default_as(old_type->name, nullptr)) {
      write_diff_type(diff, options, *old_type, *new_type);
    }
    else {
      diff.remove(fmt::format("Type: {}", old_type->name));
    }
  }
  for (const Type *new_type : new_sdna.types) {
    const Type *old_type = old_sdna.types.lookup_key_default_as(new_type->name, nullptr);
    if (old_type) {
      continue;
    }
    diff.add(fmt::format("Type: {}", new_type->name));
  }
  return diff;
}

enum class ContextType {
  Member,
  CollectionIndex,
  CollectionIdentifier,
};

struct Context {
  const Context *parent = nullptr;
  StringRefNull name;
  bool is_in_collection = false;

  std::string to_string() const
  {
    Vector<const Context *> contexts;
    for (const Context *context = this; context; context = context->parent) {
      contexts.append(context);
    }
    std::reverse(contexts.begin(), contexts.end());
    std::string result;
    result += contexts[0]->name;
    for (const Context *context : contexts.as_span().drop_front(1)) {
      if (context->is_in_collection) {
        result += fmt::format("[{}]", context->name);
      }
      else {
        result += fmt::format(".{}", context->name);
      }
    }
    return result;
  }
};

struct DataWithStruct {
  const void *data = nullptr;
  const Struct *sdna_struct = nullptr;
};

struct BlockMatch {
  const BlendBlock *old_block = nullptr;
  const BlendBlock *new_block = nullptr;

  const Context *old_context = nullptr;
  const Context *new_context = nullptr;
};

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

[[maybe_unused]] static std::pair<std::string, std::string> format_string_elements_left_align(
    const Span<std::string> values_a, const Span<std::string> values_b)
{
  BLI_assert(values_a.size() == values_b.size());
  std::string result_a;
  std::string result_b;
  for (const int64_t i : values_a.index_range()) {
    const StringRef next_a = values_a[i];
    const StringRef next_b = values_b[i];
    result_a += next_a;
    result_b += next_b;
    const int64_t end_length = std::max(result_a.size(), result_b.size()) + (i == 0 ? 0 : 1);
    result_a.append(end_length - result_a.size(), ' ');
    result_b.append(end_length - result_b.size(), ' ');
  }
  return {result_a, result_b};
}

static std::pair<std::string, std::string> format_string_elements_right_align(
    const Span<std::string> &values_a, const Span<std::string> &values_b)
{
  std::string result_a;
  std::string result_b;
  for (size_t i = 0; i < values_a.size(); i++) {
    const StringRef next_a = values_a[i];
    const StringRef next_b = values_b[i];
    const int64_t end_length = std::max(result_a.size() + next_a.size(),
                                        result_b.size() + next_b.size()) +
                               (i == 0 ? 0 : 1);
    result_a.append(end_length - result_a.size() - next_a.size(), ' ');
    result_b.append(end_length - result_b.size() - next_b.size(), ' ');
    result_a += next_a;
    result_b += next_b;
  }
  return {result_a, result_b};
}

static std::pair<std::string, std::string> format_string_elements_char_align(
    const Span<std::string> &values_a,
    const Span<std::string> &values_b,
    const char align_char = '.')
{
  std::string result_a;
  std::string result_b;
  for (size_t i = 0; i < values_a.size(); i++) {
    const StringRef next_a = values_a[i];
    const StringRef next_b = values_b[i];
    int64_t align_i_a = next_a.find(align_char);
    if (align_i_a == -1) {
      align_i_a = next_a.size();
    }
    int64_t align_i_b = next_b.find(align_char);
    if (align_i_b == -1) {
      align_i_b = next_b.size();
    }
    const int64_t align_length = std::max(result_a.size() + align_i_a,
                                          result_b.size() + align_i_b) +
                                 (i == 0 ? 0 : 1);
    result_a.append(align_length - result_a.size() - align_i_a, ' ');
    result_b.append(align_length - result_b.size() - align_i_b, ' ');
    result_a += next_a;
    result_b += next_b;
  }
  return {result_a, result_b};
}

class DiffLog {
 public:
  DiffLines &diff_;

  DiffLog(DiffLines &diff) : diff_(diff) {}

  void change_string(const Context &old_context,
                     const Context &new_context,
                     const StringRef old_str,
                     const StringRef new_str)
  {
    diff_.change(fmt::format("{} = \"{}\"", old_context.to_string(), old_str),
                 fmt::format("{} = \"{}\"", new_context.to_string(), new_str));
  }

  void change_type(const Context &old_context,
                   const Context &new_context,
                   const StringRef old_type,
                   const StringRef new_type)
  {
    diff_.change(fmt::format("{} <type> = {}", old_context.to_string(), old_type),
                 fmt::format("{} <type> = {}", new_context.to_string(), new_type));
  }

  void change_primitive_value(const Context &old_context,
                              const Context &new_context,
                              const PrimitiveValue old_value,
                              const PrimitiveValue new_value)
  {
    diff_.change(
        fmt::format("{} = {}", old_context.to_string(), primitive_value_to_string(old_value)),
        fmt::format("{} = {}", new_context.to_string(), primitive_value_to_string(new_value)));
  }

  void change_value(const Context &old_context,
                    const Context &new_context,
                    const StringRef old_value,
                    const StringRef new_value)
  {
    const std::string old_line = fmt::format("{} = {}", old_context.to_string(), old_value);
    const std::string new_line = fmt::format("{} = {}", new_context.to_string(), new_value);
    diff_.change(old_line, new_line);
  }

  void change_list_size(const Context &old_context,
                        const Context &new_context,
                        const int64_t old_size,
                        const int64_t new_size,
                        const bool order_changed)
  {
    diff_.change(fmt::format("{} <length> = {}", old_context.to_string(), old_size),
                 fmt::format("{} <length> = {}{}",
                             new_context.to_string(),
                             new_size,
                             order_changed ? " (order changed)" : ""));
  }

  void info(const Context &context, const StringRef info)
  {
    diff_.info(fmt::format("{}: {}", context.to_string(), info));
  }

  void info_list_duplicate(const Context context,
                           const StringRef identifier,
                           const StringRef user_identifier)
  {
    diff_.info(fmt::format(
        "{}: Duplicate in List: {} ({})", context.to_string(), identifier, user_identifier));
  }

  void add(const Context &context, const StringRef value)
  {
    diff_.add(fmt::format("{} = {}", context.to_string(), value));
  }

  void remove(const Context &context, const StringRef value)
  {
    diff_.remove(fmt::format("{} = {}", context.to_string(), value));
  }
};

class IdDiffer {
 private:
  ResourceScope scope_;
  DiffLog diff_log_;
  const DiffOptions &options_;

  struct PerBlendData {
    const BlendQuery &blend;
    const BlendId &id_data;
  };

  PerBlendData old_;
  PerBlendData new_;

  Map<const BlendBlock *, const BlendBlock *> old_by_new_;
  Map<const BlendBlock *, const BlendBlock *> new_by_old_;

  Stack<BlockMatch> matches_to_process_;

  struct Pointee {
    const BlendBlock *block = nullptr;
    const BlendId *id_data = nullptr;

    Pointee() = default;
    Pointee(const BlendBlock &block) : block(&block) {}
    Pointee(const BlendId &id_data) : block(id_data.id_block), id_data(&id_data) {}

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
  IdDiffer(DiffLines &diff,
           const DiffOptions &options,
           const BlendQuery &old_blend,
           const BlendQuery &new_blend,
           const BlendId &old_id_data,
           const BlendId &new_id_data)
      : diff_log_(diff),
        options_(options),
        old_{old_blend, old_id_data},
        new_{new_blend, new_id_data}
  {
  }

  const Context &init_context(const Context *parent,
                              const StringRef name,
                              const bool is_in_collection)
  {
    return scope_.construct<Context>(Context{
        .parent = parent,
        .name = scope_.allocator().copy_string(name),
        .is_in_collection = is_in_collection,
    });
  }

  void run()
  {
    const Context &old_root_context = this->init_context(
        nullptr,
        fmt::format("{}[\"{}\"]", old_.id_data.sdna_struct->type->name, old_.id_data.name.c_str()),
        false);
    const Context &new_root_context = this->init_context(
        nullptr,
        fmt::format("{}[\"{}\"]", new_.id_data.sdna_struct->type->name, new_.id_data.name.c_str()),
        false);
    matches_to_process_.push(
        {old_.id_data.id_block, new_.id_data.id_block, &old_root_context, &new_root_context});

    while (!matches_to_process_.is_empty()) {
      const BlockMatch match = matches_to_process_.pop();

      if (match.old_block->bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX &&
          match.new_block->bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX)
      {
        this->diff_raw_buffer(
            *match.old_block, *match.new_block, *match.old_context, *match.new_context);
        continue;
      }

      const Struct *old_struct = old_.blend.sdna().sdna->try_find_struct(
          match.old_block->bhead.SDNAnr);
      const Struct *new_struct = new_.blend.sdna().sdna->try_find_struct(
          match.new_block->bhead.SDNAnr);
      if (!old_struct || !new_struct) {
        continue;
      }
      if (match.old_block->bhead.nr == 1 && match.new_block->bhead.nr == 1) {
        this->diff_struct(*match.old_block,
                          *match.new_block,
                          0,
                          0,
                          *old_struct,
                          *new_struct,
                          *match.old_context,
                          *match.new_context);
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
                                *match.old_context,
                                *match.new_context);
      }
    }
  }

  const CPPType *cpp_type_from_primitive_type(const PrimitiveType type) const
  {
    switch (type) {
      case PrimitiveType::Char:
        return &CPPType::get<int8_t>();
      case PrimitiveType::UChar:
        return &CPPType::get<uchar>();
      case PrimitiveType::Short:
        return &CPPType::get<short>();
      case PrimitiveType::UShort:
        return &CPPType::get<ushort>();
      case PrimitiveType::Int:
        return &CPPType::get<int>();
      case PrimitiveType::Float:
        return &CPPType::get<float>();
      case PrimitiveType::Double:
        return &CPPType::get<double>();
      case PrimitiveType::Int64:
        return &CPPType::get<int64_t>();
      case PrimitiveType::UInt64:
        return &CPPType::get<uint64_t>();
      case PrimitiveType::Int8:
        return &CPPType::get<int8_t>();
    }
    BLI_assert_unreachable();
    return nullptr;
  }

  int pointer_level_from_name(const StringRef name) const
  {
    int64_t level = 0;
    for (const char c : name) {
      if (c == '*') {
        level++;
      }
    }
    return level;
  }

  void diff_raw_buffer(const BlendBlock &old_block,
                       const BlendBlock &new_block,
                       const Context &old_context,
                       const Context &new_context)
  {
    if (!old_block.type || !new_block.type) {
      return;
    }
    const int pointer_level = old_block.type->pointer_level;
    if (pointer_level != new_block.type->pointer_level) {
      return;
    }
    if (pointer_level == 0) {
      if (old_block.type->sdna_base_type && new_block.type->sdna_base_type) {
        const Type &old_sdna_type = *old_block.type->sdna_base_type;
        const Type &new_sdna_type = *new_block.type->sdna_base_type;
        if (old_sdna_type.name != new_sdna_type.name) {
          return;
        }
        if (ELEM(0, old_sdna_type.size_in_bytes, new_sdna_type.size_in_bytes)) {
          return;
        }
        if (old_block.bhead.len % old_sdna_type.size_in_bytes != 0) {
          return;
        }
        if (new_block.bhead.len % new_sdna_type.size_in_bytes != 0) {
          return;
        }
        const int64_t old_num = old_block.bhead.len / old_sdna_type.size_in_bytes;
        const int64_t new_num = new_block.bhead.len / new_sdna_type.size_in_bytes;
        if (old_sdna_type.opt_struct && new_sdna_type.opt_struct) {
          this->diff_struct_array(old_block,
                                  new_block,
                                  0,
                                  0,
                                  *old_sdna_type.opt_struct,
                                  *new_sdna_type.opt_struct,
                                  old_num,
                                  new_num,
                                  old_context,
                                  new_context);
        }
      }
      if (old_block.type->cpp_base_type && new_block.type->cpp_base_type) {
        const CPPType &old_cpp_type = *old_block.type->cpp_base_type;
        const CPPType &new_cpp_type = *new_block.type->cpp_base_type;
        if (old_cpp_type != new_cpp_type) {
          return;
        }
        const CPPType &cpp_type = old_cpp_type;
        if (old_block.bhead.len % cpp_type.size != 0) {
          return;
        }
        if (new_block.bhead.len % cpp_type.size != 0) {
          return;
        }
        const int64_t old_num = old_block.bhead.len / cpp_type.size;
        const int64_t new_num = new_block.bhead.len / cpp_type.size;
        this->diff_GSpan(GSpan{cpp_type, old_block.data, old_num},
                         GSpan{cpp_type, new_block.data, new_num},
                         old_context,
                         new_context);
      }
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
        const uint64_t address = blend_query::read_address(old_block.data + i * pointer_size);
        old_pointees.append(this->lookup_pointee(old_, address));
      }
      for (const int64_t i : IndexRange(new_num)) {
        const uint64_t address = blend_query::read_address(new_block.data + i * pointer_size);
        new_pointees.append(this->lookup_pointee(new_, address));
      }
      this->diff_block_list(old_pointees, new_pointees, old_context, new_context);
      return;
    }
  }

  void diff_struct(const BlendBlock &old_block,
                   const BlendBlock &new_block,
                   const int64_t old_struct_offset,
                   const int64_t new_struct_offset,
                   const Struct &old_struct,
                   const Struct &new_struct,
                   const Context &old_context,
                   const Context &new_context)
  {
    const StringRef type_name = old_struct.type->name;
    if (type_name != new_struct.type->name) {
      diff_log_.change_type(
          old_context, new_context, old_struct.type->name, new_struct.type->name);
      return;
    }
    if (type_name == "ListBase") {
      this->diff_ListBase(
          old_block, new_block, old_struct_offset, new_struct_offset, old_context, new_context);
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
                            old_context,
                            new_context);
    }

    for (const StructMember *old_member : old_struct.members) {
      const StructMember *new_member = new_struct.members.lookup_key_default_as(old_member->name,
                                                                                nullptr);
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
                               old_context,
                               new_context);
    }
  }

  void diff_struct_member(const BlendBlock &old_block,
                          const BlendBlock &new_block,
                          const int64_t old_member_offset,
                          const int64_t new_member_offset,
                          const StructMember &old_member,
                          const StructMember &new_member,
                          const Context &old_context,
                          const Context &new_context)
  {
    const StringRef type_name = old_member.type->name;
    const StructMember::Category category = old_member.category;
    const int64_t elem_num = old_member.elem_num;
    const StringRef name = old_member.name;
    const std::optional<PrimitiveType> opt_primitive_type = old_member.type->opt_primitive_type;
    if (new_member.type->name != type_name || new_member.category != category ||
        new_member.elem_num != elem_num || new_member.name != name ||
        new_member.type->opt_primitive_type != opt_primitive_type)
    {
      /* This is in the diff as part of SDNA changes, no need to mention it for every block. */
      return;
    }
    if (options_.ignore_member(new_member)) {
      return;
    }
    const Context &old_member_context = this->init_context(&old_context, name, false);
    const Context &new_member_context = this->init_context(&new_context, name, false);
    const bool is_array = elem_num > 1;
    switch (category) {
      case rich_sdna::StructMember::Category::Struct: {
        const Struct &old_substruct = *old_member.type->opt_struct;
        const Struct &new_substruct = *new_member.type->opt_struct;
        for (const int i : IndexRange(elem_num)) {
          const Context *old_sub_context = &old_member_context;
          const Context *new_sub_context = &new_member_context;
          if (is_array) {
            old_sub_context = &this->init_context(&old_member_context, std::to_string(i), true);
            new_sub_context = &this->init_context(&new_member_context, std::to_string(i), true);
          }
          this->diff_struct(old_block,
                            new_block,
                            old_member_offset + i * old_member.elem_size,
                            new_member_offset + i * new_member.elem_size,
                            old_substruct,
                            new_substruct,
                            *old_sub_context,
                            *new_sub_context);
        }
        break;
      }
      case rich_sdna::StructMember::Category::Primitive: {
        BLI_assert(opt_primitive_type.has_value());
        const PrimitiveType primitive_type = *opt_primitive_type;
        if (primitive_type == PrimitiveType::Char) {
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
            diff_log_.change_string(old_member_context, new_member_context, *old_str, *new_str);
            break;
          }
        }
        if (elem_num > 1) {
          if (const CPPType *cpp_type = this->cpp_type_from_primitive_type(primitive_type)) {
            const GSpan old_values{
                *cpp_type,
                reinterpret_cast<const float *>(old_block.data + old_member_offset),
                elem_num};
            const GSpan new_values{
                *cpp_type,
                reinterpret_cast<const float *>(new_block.data + new_member_offset),
                elem_num};
            this->diff_GSpan(old_values, new_values, old_member_context, new_member_context);
            break;
          }
        }
        for (const int i : IndexRange(elem_num)) {
          const PrimitiveValue old_value = blend_query::read_primitive_value(
              primitive_type, old_block.data + old_member_offset + i * old_member.elem_size);
          const PrimitiveValue new_value = blend_query::read_primitive_value(
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
          if (this->consider_primitive_values_equal(old_value, new_value)) {
            continue;
          }
          const Context *old_sub_context = &old_member_context;
          const Context *new_sub_context = &new_member_context;
          if (is_array) {
            old_sub_context = &this->init_context(&old_member_context, std::to_string(i), true);
            new_sub_context = &this->init_context(&new_member_context, std::to_string(i), true);
          }
          diff_log_.change_primitive_value(
              *old_sub_context, *new_sub_context, old_value, new_value);
        }
        break;
      }
      case rich_sdna::StructMember::Category::Pointer: {
        for (const int i : IndexRange(elem_num)) {
          const uint64_t old_address = blend_query::read_address(
              old_block.data + old_member_offset + i * old_member.elem_size);
          const uint64_t new_address = blend_query::read_address(
              new_block.data + new_member_offset + i * new_member.elem_size);
          const Pointee old_pointee = this->lookup_pointee(old_, old_address);
          const Pointee new_pointee = this->lookup_pointee(new_, new_address);
          if (!old_pointee && !new_pointee) {
            continue;
          }
          const Context *old_sub_context = &old_member_context;
          const Context *new_sub_context = &new_member_context;
          if (is_array) {
            old_sub_context = &this->init_context(&old_member_context, std::to_string(i), true);
            new_sub_context = &this->init_context(&new_member_context, std::to_string(i), true);
          }
          PointeeToStringOptions options;
          options.include_identifier = true;
          const std::string old_pointee_str = this->pointee_to_string(old_, old_pointee, options);
          const std::string new_pointee_str = this->pointee_to_string(new_, new_pointee, options);
          if (old_pointee_str != new_pointee_str) {
            diff_log_.change_value(
                *old_sub_context, *new_sub_context, old_pointee_str, new_pointee_str);
          }
          if (old_pointee.id_data || new_pointee.id_data) {
            continue;
          }
          if (options_.dont_follow_members.contains(new_member)) {
            continue;
          }
          if (old_pointee.block && new_pointee.block) {
            this->tag_potentially_corresponding_blocks(
                *old_pointee.block, *new_pointee.block, *old_sub_context, *new_sub_context);
          }
        }
        break;
      }
    }
  }

  bool consider_primitive_values_equal(const PrimitiveValue value_a, const PrimitiveValue value_b)
  {
    BLI_assert(value_a.index() == value_b.index());
    return std::visit(
        [&](const auto &a) {
          using T = std::decay_t<decltype(a)>;
          const auto &b = std::get<T>(value_b);
          if constexpr (is_same_any_v<T, float, double>) {
            return std::abs(a - b) < 0.000001f;
          }
          return a == b;
        },
        value_a);
  }

  void diff_ListBase(const BlendBlock &old_block,
                     const BlendBlock &new_block,
                     const int64_t old_struct_offset,
                     const int64_t new_struct_offset,
                     const Context &old_context,
                     const Context &new_context)
  {
    const uint64_t old_first_address = *reinterpret_cast<const uint64_t *>(old_block.data +
                                                                           old_struct_offset);
    const uint64_t new_first_address = *reinterpret_cast<const uint64_t *>(new_block.data +
                                                                           new_struct_offset);
    const Vector<Pointee> old_pointees = this->gather_linked_list_pointees(old_first_address,
                                                                           old_);
    const Vector<Pointee> new_pointees = this->gather_linked_list_pointees(new_first_address,
                                                                           new_);
    this->diff_block_list(old_pointees, new_pointees, old_context, new_context);
  }

  void diff_struct_array(const BlendBlock &old_block,
                         const BlendBlock &new_block,
                         const int64_t old_offset,
                         const int64_t new_offset,
                         const Struct &old_struct,
                         const Struct &new_struct,
                         const int64_t old_num,
                         const int64_t new_num,
                         const Context &old_context,
                         const Context &new_context)
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
    this->diff_struct_list(
        old_block, new_block, old_structs, new_structs, old_context, new_context);
  }

  void diff_struct_list(const BlendBlock &old_block,
                        const BlendBlock &new_block,
                        const Span<DataWithStruct> old_structs,
                        const Span<DataWithStruct> new_structs,
                        const Context &old_context,
                        const Context &new_context)
  {
    struct Item {
      DataWithStruct data_with_struct;
      int64_t index;
    };

    bool old_has_any_non_index_identifier = false;

    Map<std::string, Item> old_struct_map;
    for (const int64_t i : old_structs.index_range()) {
      const DataWithStruct &old_struct = old_structs[i];
      std::optional<std::string> identifier = this->get_struct_identifier(
          old_, old_struct.data, *old_struct.sdna_struct, false);
      if (identifier) {
        old_has_any_non_index_identifier = true;
      }
      else {
        identifier = this->get_index_fallback_identifier(i);
      }
      if (!old_struct_map.add(*identifier, {old_struct, i})) {
        const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
            old_, old_struct.data, *old_struct.sdna_struct, i, true);
        diff_log_.info_list_duplicate(new_context, *identifier, user_identifier);
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
        diff_log_.info_list_duplicate(new_context, identifier, user_identifier);
      }
      if (const Item *old_item = old_struct_map.lookup_ptr(identifier)) {
        const DataWithStruct &old_struct = old_item->data_with_struct;
        const Context &old_sub_context = this->init_context(&old_context, user_identifier, true);
        const Context &new_sub_context = this->init_context(&new_context, user_identifier, true);
        this->diff_struct(old_block,
                          new_block,
                          intptr_t(old_struct.data) - intptr_t(old_block.data),
                          intptr_t(new_struct.data) - intptr_t(new_block.data),
                          *old_struct.sdna_struct,
                          *new_struct.sdna_struct,
                          old_sub_context,
                          new_sub_context);
        if (i != old_item->index) {
          order_changed = true;
        }
      }
      else {
        const Context &new_sub_context = this->init_context(&new_context, user_identifier, true);
        diff_log_.add(new_sub_context, fmt::format("{}(...)", new_struct.sdna_struct->type->name));
      }
    }

    if (old_has_any_non_index_identifier) {
      for (const int64_t i : old_structs.index_range()) {
        const DataWithStruct &old_struct = old_structs[i];
        const std::string identifier = this->get_struct_identifier_with_index_fallback(
            old_, old_struct.data, *old_struct.sdna_struct, i);
        if (new_struct_map.contains(identifier)) {
          continue;
        }
        const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
            old_, old_struct.data, *old_struct.sdna_struct, i, true);
        const Context &old_sub_context = this->init_context(&old_context, user_identifier, true);
        diff_log_.remove(old_sub_context,
                         fmt::format("{}(...)", old_struct.sdna_struct->type->name));
      }
    }

    if (old_structs.size() != new_structs.size() || order_changed) {
      diff_log_.change_list_size(
          old_context, new_context, old_structs.size(), new_structs.size(), order_changed);
    }
  }

  void diff_block_list(const Span<Pointee> old_pointees,
                       const Span<Pointee> new_pointees,
                       const Context &old_context,
                       const Context &new_context)
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
      const Struct &old_struct = *old_.blend.sdna().sdna->try_find_struct(
          old_pointee.block->bhead.SDNAnr);
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_pointee.block->data, old_struct, i);
      if (!old_pointee_map.add(identifier, {old_pointee, i})) {
        const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
            old_, old_pointee.block->data, old_struct, i, true);
        diff_log_.info_list_duplicate(new_context, identifier, user_identifier);
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
      const Struct &new_struct = *new_.blend.sdna().sdna->try_find_struct(
          new_pointee.block->bhead.SDNAnr);
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          new_, new_pointee.block->data, new_struct, i);
      const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
          new_, new_pointee.block->data, new_struct, i, true);
      if (!new_pointee_map.add(identifier, {new_pointee, i})) {
        diff_log_.info_list_duplicate(new_context, identifier, user_identifier);
      }
      if (const Item *old_item = old_pointee_map.lookup_ptr(identifier)) {
        if (!new_pointee.id_data && !old_item->pointee.id_data) {
          const BlendBlock &old_pointee = *old_item->pointee.block;
          const Context &old_sub_context = this->init_context(&old_context, user_identifier, true);
          const Context &new_sub_context = this->init_context(&new_context, user_identifier, true);
          this->tag_potentially_corresponding_blocks(
              old_pointee, *new_pointee.block, old_sub_context, new_sub_context);
        }
        if (i != old_item->index) {
          order_changed = true;
        }
      }
      else {
        const Context &new_sub_context = this->init_context(&new_context, user_identifier, true);
        diff_log_.add(new_sub_context,
                      this->pointee_to_string(new_, new_pointee, pointee_to_string_options));
      }
    }

    for (const int64_t i : old_pointees.index_range()) {
      const Pointee old_pointee = old_pointees[i];
      if (!old_pointee) {
        continue;
      }
      const Struct &old_struct = *old_.blend.sdna().sdna->try_find_struct(
          old_pointee.block->bhead.SDNAnr);
      const std::string identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_pointee.block->data, old_struct, i);
      if (new_pointee_map.contains(identifier)) {
        continue;
      }
      const std::string user_identifier = this->get_struct_identifier_with_index_fallback(
          old_, old_pointee.block->data, old_struct, i, true);
      const Context &old_sub_context = this->init_context(&old_context, user_identifier, true);
      diff_log_.remove(old_sub_context,
                       this->pointee_to_string(old_, old_pointee, pointee_to_string_options));
    }

    if (old_pointees.size() != new_pointees.size() || order_changed) {
      diff_log_.change_list_size(
          old_context, new_context, old_pointees.size(), new_pointees.size(), order_changed);
    }
  }

  void diff_GSpan(const GSpan old_span,
                  const GSpan new_span,
                  const Context &old_context,
                  const Context &new_context)
  {
    if (old_span.size() != new_span.size()) {
      diff_log_.change_list_size(
          old_context, new_context, old_span.size(), new_span.size(), false);
      return;
    }
    const CPPType &type = old_span.type();
    if (!type.is_equality_comparable()) {
      return;
    }
    const int64_t elem_num = old_span.size();
    Vector<int64_t> changed_indices;
    for (const int64_t i : IndexRange(elem_num)) {
      const void *old_value = old_span[i];
      const void *new_value = new_span[i];
      if (!type.is_equal(old_value, new_value)) {
        changed_indices.append(i);
      }
    }
    if (changed_indices.is_empty()) {
      return;
    }
    if (elem_num <= 16) {
      Vector<std::string> old_value_strings;
      Vector<std::string> new_value_strings;
      for (const int64_t i : IndexRange(elem_num)) {
        old_value_strings.append(type.to_string(old_span[i]));
        new_value_strings.append(type.to_string(new_span[i]));
      }
      const std::pair<std::string, std::string> value_strings =
          type.is_any<float>() ?
              format_string_elements_char_align(old_value_strings, new_value_strings) :
              format_string_elements_right_align(old_value_strings, new_value_strings);
      diff_log_.diff_.change(
          fmt::format("{} = [{}]", old_context.to_string(), value_strings.first),
          fmt::format("{} = [{}]", new_context.to_string(), value_strings.second));
      return;
    }
    if (changed_indices.size() > options_.max_array_changes) {
      diff_log_.diff_.change(
          fmt::format(
              "{} <length> = {}x {}", old_context.to_string(), old_span.size(), type.name()),
          fmt::format("{} <length> = {}x {} ({} indices changed)",
                      new_context.to_string(),
                      new_span.size(),
                      type.name(),
                      changed_indices.size()));
      return;
    }
    for (const int64_t i : changed_indices) {
      const void *old_value = old_span[i];
      const void *new_value = new_span[i];
      const Context &old_sub_context = this->init_context(&old_context, std::to_string(i), true);
      const Context &new_sub_context = this->init_context(&new_context, std::to_string(i), true);
      diff_log_.change_value(
          old_sub_context, new_sub_context, type.to_string(old_value), type.to_string(new_value));
    }
  }

  void diff_IDProperty(const BlendBlock &old_block,
                       const BlendBlock &new_block,
                       const int old_struct_offset,
                       const int new_struct_offset,
                       const Struct &old_IDProperty,
                       const Struct &new_IDProperty,
                       const Context &old_context,
                       const Context &new_context)
  {
    const BlendValue old_prop{&old_.id_data,
                              MemType::from_sdna_type(*old_IDProperty.type),
                              1,
                              old_block.data + old_struct_offset};
    const BlendValue new_prop{&new_.id_data,
                              MemType::from_sdna_type(*new_IDProperty.type),
                              1,
                              new_block.data + new_struct_offset};

    const std::optional<char> old_type = old_.blend.lookup(old_prop, "type").as_primitive<char>();
    const std::optional<char> new_type = new_.blend.lookup(new_prop, "type").as_primitive<char>();
    if (!old_type || !new_type) {
      return;
    }
    if (!ELEM(old_type, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN)) {
      return;
    }
    if (!ELEM(new_type, IDP_INT, IDP_FLOAT, IDP_DOUBLE, IDP_BOOLEAN)) {
      return;
    }
    const std::optional<int> old_val =
        old_.blend.lookup(old_prop, {"data", "val"}).as_primitive<int>();
    const std::optional<int> old_val2 =
        old_.blend.lookup(old_prop, {"data", "val2"}).as_primitive<int>();
    const std::optional<int> new_val =
        new_.blend.lookup(new_prop, {"data", "val"}).as_primitive<int>();
    const std::optional<int> new_val2 =
        new_.blend.lookup(new_prop, {"data", "val2"}).as_primitive<int>();
    if (!old_val || !old_val2 || !new_val || !new_val2) {
      return;
    }
    const PrimitiveValue old_value = blend_query::decode_primitive_id_property_value(
        eIDPropertyType(*old_type), *old_val, *old_val2);
    const PrimitiveValue new_value = blend_query::decode_primitive_id_property_value(
        eIDPropertyType(*new_type), *new_val, *new_val2);
    if (old_value == new_value) {
      return;
    }
    const Context &old_sub_context = this->init_context(&old_context, "decoded_value", true);
    const Context &new_sub_context = this->init_context(&new_context, "decoded_value", true);
    diff_log_.change_primitive_value(old_sub_context, new_sub_context, old_value, new_value);
  }

  Vector<Pointee> gather_linked_list_pointees(const uint64_t first_address,
                                              const PerBlendData &blend_data)
  {
    Vector<Pointee> pointees;
    uint64_t next_address = first_address;
    while (const Pointee pointee = this->lookup_pointee(blend_data, next_address)) {
      const Struct *sdna_struct = blend_data.blend.sdna().sdna->try_find_struct(
          pointee.block->bhead.SDNAnr);
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
    if (const BlendBlock *block = blend_data.id_data.lookup_internal_block(address)) {
      return *block;
    }
    if (const BlendId *id_data = blend_data.blend.lookup_id(address)) {
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
    const uint64_t address = blend_query::read_address(
        POINTER_OFFSET(data, member->offset_in_struct));
    return blend_data.id_data.lookup_internal_block(address);
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
    if (const BlendId *id_data = pointee.id_data) {
      return fmt::format("{}[\"{}\"]", id_data->sdna_struct->type->name, id_data->name);
    }
    const BlendBlock &block = *pointee.block;
    if (block.bhead.SDNAnr == SDNA_RAW_DATA_STRUCT_INDEX) {
      const Span<char> bytes{block.data, block.bhead.len};
      if (block.type) {
        if (block.type->sdna_base_type) {
          if (block.type->pointer_level == 0 && block.type->sdna_base_type->name == "char" &&
              bytes.size() <= 128)
          {
            if (std::optional<std::string> str = try_convert_char_array_to_readable_string(bytes))
            {
              return fmt::format("\"{}\"", *str);
            }
          }
          if (block.type->pointer_level == 1) {
            return fmt::format(
                "{}x {}", block.bhead.len / sizeof(void *), block.type->sdna_base_type->name);
          }
          if (block.type->pointer_level == 2) {
            return fmt::format(
                "{}x {} *", block.bhead.len / sizeof(void *), block.type->sdna_base_type->name);
          }
        }
      }
      const uint64_t hash = XXH3_64bits(bytes.data(), bytes.size());
      char size_buf[BLI_STR_FORMAT_INT64_BYTE_UNIT_SIZE];
      BLI_str_format_byte_unit(size_buf, bytes.size(), true);
      return fmt::format("hash({}) -> 0x{:x}", size_buf, hash);
    }
    if (const Struct *sdna_struct = blend_data.blend.sdna().sdna->try_find_struct(
            block.bhead.SDNAnr))
    {
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
                                            const Context &old_context,
                                            const Context &new_context)
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
    const Struct *old_struct = old_.blend.sdna().sdna->try_find_struct(old_block.bhead.SDNAnr);
    const Struct *new_struct = new_.blend.sdna().sdna->try_find_struct(new_block.bhead.SDNAnr);
    if (old_struct && new_struct) {
      if (old_struct->type->name != new_struct->type->name) {
        return;
      }
    }
    /* Blocks with different identifiers cannot be matched. */
    if (this->data_blocks_have_consistent_identifier(old_block, new_block).value_or(true) == false)
    {
      return;
    }
    old_by_new_.add_new(&new_block, &old_block);
    new_by_old_.add_new(&old_block, &new_block);
    matches_to_process_.push({&old_block, &new_block, &old_context, &new_context});
  }

  std::optional<bool> data_blocks_have_consistent_identifier(const BlendBlock &old_block,
                                                             const BlendBlock &new_block)
  {
    if (old_block.bhead.nr >= 2 || new_block.bhead.nr >= 2) {
      return std::nullopt;
    }
    const Struct *old_struct = old_.blend.sdna().sdna->try_find_struct(old_block.bhead.SDNAnr);
    const Struct *new_struct = new_.blend.sdna().sdna->try_find_struct(new_block.bhead.SDNAnr);
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
    const BlendValue bstruct{&blend_data.id_data,
                             MemType::from_sdna_type(*sdna_struct.type),
                             1,
                             static_cast<const char *>(data)};
    if (is_specific_id_struct(sdna_struct)) {
      return blend_data.blend.lookup(bstruct, "name").as_string();
    }
    if (!sdna_struct.members.is_empty()) {
      const StructMember &first_member = *sdna_struct.members[0];
      if (first_member.category == StructMember::Category::Struct) {
        if (first_member.type->name == "ModifierData") {
          if (ui_identifier) {
            return blend_data.blend.lookup(bstruct, {"modifier", "name"}).as_string();
          }
          if (const std::optional<int> identifier = blend_data.blend
                                                        .lookup(bstruct,
                                                                {"modifier", "persistent_uid"})
                                                        .as_primitive<int>())
          {
            return fmt::format("id:{}", *identifier);
          }
        }
      }
    }

    if (sdna_struct.type->name == "bNode") {
      if (!ui_identifier) {
        if (const std::optional<int> identifier =
                blend_data.blend.lookup(bstruct, "identifier").as_primitive<int>())
        {
          return fmt::format("id:{}", *identifier);
        }
      }
    }
    if (sdna_struct.type->name == "bNodeSocket") {
      if (!ui_identifier) {
        return blend_data.blend.lookup(bstruct, "identifier").as_string();
      }
    }
    if (sdna_struct.type->name == "bNodeLink") {
      return this->get_bNodeLink_identifier(blend_data, data, sdna_struct, ui_identifier);
    }
    if (sdna_struct.type->name == "Attribute") {
      return blend_data.blend.lookup(bstruct, {"name", blend_query::Deref()}).as_string();
    }
    if (sdna_struct.type->name == "IDProperty") {
      return blend_data.blend.lookup(bstruct, blend_query::LookupPathElem{"name"}).as_string();
    }
    if (sdna_struct.type->name == "bNodeTreeInterfacePanel") {
      if (!ui_identifier) {
        if (const std::optional<int> identifier =
                blend_data.blend.lookup(bstruct, "identifier").as_primitive<int>())
        {
          return fmt::format("id:{}", *identifier);
        }
      }
    }
    if (sdna_struct.type->name == "bNodeTreeInterfaceSocket") {
      if (!ui_identifier) {
        return blend_data.blend.lookup(bstruct, {"identifier", blend_query::Deref()}).as_string();
      }
    }
    if (ui_identifier) {
      if (std::optional<StringRefNull> name = blend_data.blend.lookup(bstruct, "name").as_string())
      {
        return name;
      }
      if (std::optional<StringRefNull> name =
              blend_data.blend.lookup(bstruct, {"name", blend_query::Deref()}).as_string())
      {
        return name;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> get_bNodeLink_identifier(const PerBlendData &blend_data,
                                                      const void *data,
                                                      const Struct &sdna_struct,
                                                      const bool ui_identifier) const
  {
    const BlendBlock *from_node = this->lookup_local_data(
        blend_data, data, sdna_struct, "fromnode", "bNode");
    const BlendBlock *to_node = this->lookup_local_data(
        blend_data, data, sdna_struct, "tonode", "bNode");
    const BlendBlock *from_socket = this->lookup_local_data(
        blend_data, data, sdna_struct, "fromsock", "bNodeSocket");
    const BlendBlock *to_socket = this->lookup_local_data(
        blend_data, data, sdna_struct, "tosock", "bNodeSocket");
    if (!from_node || !to_node || !from_socket || !to_socket) {
      return std::nullopt;
    }
    const Struct &struct_bNode = *blend_data.blend.sdna().sdna->try_find_struct("bNode");
    const Struct &struct_bNodeSocket = *blend_data.blend.sdna().sdna->try_find_struct(
        "bNodeSocket");
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

struct AllIdDiffLines {
  DiffLines removed_ids;
  DiffLines added_ids;
  DiffLines ignored_ids;
  Vector<std::pair<std::string, DiffLines>> changed_ids;
};

struct IdKey {
  StringRef name;
  StringRef type_name;

  IdKey(const BlendId &id) : name(id.name), type_name(id.sdna_struct->type->name) {}

  uint64_t hash() const
  {
    return get_default_hash(this->name, this->type_name);
  }

  bool operator==(const IdKey &other) const = default;
};

static AllIdDiffLines write_diff_ids(const DiffOptions &options,
                                     const BlendQuery &old_blend,
                                     const BlendQuery &new_blend)
{
  Map<IdKey, const BlendId *> old_id_names;
  Map<IdKey, const BlendId *> new_id_names;

  AllIdDiffLines all_diffs;

  for (const BlendId &id_data : old_blend.ids()) {
    old_id_names.add(id_data, &id_data);
  }
  for (const BlendId &id_data : new_blend.ids()) {
    new_id_names.add(id_data, &id_data);
  }
  Vector<std::pair<const BlendId *, const BlendId *>> id_pairs;
  for (const BlendId &old_id_data : old_blend.ids()) {
    if (const BlendId *new_id_data = new_id_names.lookup_default(old_id_data, nullptr)) {
      if (options.ignore_id_type(new_id_data->sdna_struct->type->name)) {
        all_diffs.ignored_ids.info(fmt::format("Data Block ignored: \"{}\"", new_id_data->name));
      }
      else {
        id_pairs.append({&old_id_data, new_id_data});
      }
    }
    else {
      all_diffs.removed_ids.remove(fmt::format("Data-block: {}", old_id_data.name));
    }
  }
  for (const BlendId &new_id_data : new_blend.ids()) {
    if (old_id_names.contains(new_id_data)) {
      continue;
    }
    all_diffs.added_ids.add(fmt::format("Data-block: {}", new_id_data.name));
  }
  all_diffs.changed_ids.resize(id_pairs.size());
  threading::parallel_for(id_pairs.index_range(), 1, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const BlendId &old_id_data = *id_pairs[i].first;
      const BlendId &new_id_data = *id_pairs[i].second;
      DiffLines id_diff;
      IdDiffer id_differ(id_diff, options, old_blend, new_blend, old_id_data, new_id_data);
      id_differ.run();
      all_diffs.changed_ids[i].first = new_id_data.name;
      all_diffs.changed_ids[i].second = std::move(id_diff);
    }
  });
  return all_diffs;
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

static std::string get_default_config_path(const StringRefNull binary_path)
{
  char path[FILE_MAX] = "//blend_diff_config.toml";
  BLI_path_abs(path, binary_path.c_str());
  return path;
}

static int main_do(const int argc, char *argv[])
{
  register_cpp_types();

  if (argc < 3) {
    fmt::println(stderr, "Usage: blend_diff <file_old> <file_new>");
    return 1;
  }

  const StringRefNull file_old = argv[1];
  const StringRefNull file_new = argv[2];

  std::unique_ptr<BlendQuery> old_blend = BlendQuery::from_file(file_old);
  std::unique_ptr<BlendQuery> new_blend = BlendQuery::from_file(file_new);

  if (!old_blend) {
    handle_invalid_blend_file_error(file_old);
    return 1;
  }
  if (!new_blend) {
    handle_invalid_blend_file_error(file_new);
    return 1;
  }

  const std::string diff_config_path = get_default_config_path(argv[0]);
  const toml::basic_value<toml::type_config> diff_config_toml = toml::parse(
      diff_config_path, toml::spec::v(1, 1, 0));

  DiffOptions options;
  options.ignore_pad = toml::find_or<bool>(diff_config_toml, "ignore_pad", true);
  {
    const auto &ignored_members = toml::find(diff_config_toml, "ignored_members").as_table();
    for (const auto &[struct_name, members_to_ignore] : ignored_members) {
      for (const auto &member_name : members_to_ignore.as_array()) {
        options.add_member_to_ignore(struct_name, member_name.as_string());
      }
    }
  }
  {
    const auto &ignored_flags = toml::find(diff_config_toml, "ignored_flags").as_table();
    for (const auto &[struct_name, member_flags] : ignored_flags) {
      for (const auto &[member_name, member_flags] : member_flags.as_table()) {
        for (const auto &flag_bit : member_flags.as_array()) {
          options.add_ignored_flags(struct_name, member_name, (1 << flag_bit.as_integer()));
        }
      }
    }
  }
  {
    const auto &nofollow_members = toml::find(diff_config_toml, "nofollow_members").as_table();
    for (const auto &[struct_name, members_to_ignore] : nofollow_members) {
      for (const auto &member_name : members_to_ignore.as_array()) {
        options.dont_follow_members.add({struct_name, member_name.as_string()});
      }
    }
  }
  {
    const auto &ignored_id_types = toml::find(diff_config_toml, "ignored_id_types").as_array();
    for (const auto &id_type : ignored_id_types) {
      options.add_id_types_to_ignore({id_type.as_string()});
    }
  }

  const DiffLines sdna_diff = write_diff_sdna(
      options, *new_blend->sdna().sdna, *old_blend->sdna().sdna);
  const AllIdDiffLines id_diffs = write_diff_ids(options, *old_blend, *new_blend);

  auto write_output = [&](const DiffLines &diff) { std::cout << diff.to_string(); };

  for (const std::pair<std::string, DiffLines> &changed_id : id_diffs.changed_ids) {
    if (!changed_id.second.is_empty()) {
      write_output(changed_id.second);
    }
  }
  DiffLines unmodified_diff;
  for (const std::pair<std::string, DiffLines> &changed_id : id_diffs.changed_ids) {
    if (changed_id.second.is_empty()) {
      unmodified_diff.info(fmt::format("Data-block not modified: \"{}\"", changed_id.first));
    }
  }
  write_output(unmodified_diff);
  write_output(id_diffs.added_ids);
  write_output(id_diffs.removed_ids);
  write_output(id_diffs.ignored_ids);
  write_output(sdna_diff);
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
