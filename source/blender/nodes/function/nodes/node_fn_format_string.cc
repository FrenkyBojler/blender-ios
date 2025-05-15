/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <charconv>
#include <fmt/format.h>
#include <regex>

#include "RNA_enum_types.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BLO_read_write.hh"

#include "NOD_fn_format_string.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_format_string_cc {

NODE_STORAGE_FUNCS(NodeFunctionFormatString)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::String>("Format").hide_label();
  b.add_output<decl::String>("String").align_with_previous();

  const bNodeTree *ntree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (!ntree || !node) {
    return;
  }

  const NodeFunctionFormatString &storage = node_storage(*node);
  for (const int i : IndexRange(storage.items_num)) {
    const NodeFunctionFormatStringItem &item = storage.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const StringRef name = item.name;
    const std::string identifier = FormatStringItemsAccessor::socket_identifier_for_item(item);
    b.add_input(socket_type, name, identifier)
        .socket_name_ptr(&ntree->id, FormatStringItemsAccessor::item_srna, &item, "name");
  }

  b.add_input<decl::Extend>("", "__extend__");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeFunctionFormatString *data = MEM_callocN<NodeFunctionFormatString>(__func__);
  node->storage = data;
}

static void node_copy_storage(bNodeTree * /*tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeFunctionFormatString &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeFunctionFormatString>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<FormatStringItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<FormatStringItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<FormatStringItemsAccessor>(
      *ntree, *node, *node, *link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<FormatStringItemsAccessor>();
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  if (uiLayout *panel = layout->panel(C, "format_string_items", false, IFACE_("Format Items"))) {
    socket_items::ui::draw_items_list_with_operators<FormatStringItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<FormatStringItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          uiLayoutSetPropSep(panel, true);
          uiLayoutSetPropDecorate(panel, false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<FormatStringItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<FormatStringItemsAccessor>(&reader, node);
}

static std::optional<int64_t> find_format_length(const StringRef format)
{
  BLI_assert(format[0] == '{');
  int64_t braces_depth = 1;
  for (const char &c : format.substr(1)) {
    if (c == '{') {
      braces_depth++;
    }
    else if (c == '}') {
      braces_depth--;
    }
    if (braces_depth == 0) {
      return &c - format.data() + 1;
    }
  }
  return std::nullopt;
}

static int64_t find_next_format_start_or_end(const StringRef format,
                                             const int64_t start,
                                             std::string &r_out)
{
  int64_t i = start;
  while (i < format.size()) {
    const char c = format[i];
    switch (c) {
      case '{':
      case '}': {
        if (i + 1 < format.size()) {
          const char next_c = format[i + 1];
          if (next_c == c) {
            i += 2;
            r_out += c;
            continue;
          }
        }
        return i;
      }
      default: {
        r_out += c;
        i++;
        break;
      }
    }
  }
  return format.size();
}

struct FormatPatternInfo {
  std::regex pattern;
  int width_identifier_group;
  std::optional<int> precision_identifier_group;
};

/** Also see https://fmt.dev/latest/syntax/. */
static FormatPatternInfo get_pattern_by_type_impl(const CPPType &type)
{
  std::string pattern;
  int groups_num = 0;
  /* Fill and Align. */
  pattern += "([^{}]?[<>^])?";
  groups_num += 1;
  if (type.is<float>() || type.is<int>()) {
    /* Sign. */
    pattern += "[+\\- ]?";
    /* Alternate form. */
    pattern += "#?";
    /* Sign-aware zero padding. */
    pattern += "0?";
  }
  const std::string integer_or_identifier = "(\\d+|(\\{.*\\}))";
  /* Width. */
  pattern += integer_or_identifier;
  pattern += "?";
  groups_num += 2;
  const int width_identifier_group = groups_num;

  std::optional<int> precision_identifier_group;
  if (type.is<float>() || type.is<std::string>()) {
    /* Precision. */
    pattern += "(\\.";
    pattern += integer_or_identifier;
    pattern += ")?";
    groups_num += 3;
    precision_identifier_group = groups_num;
  }
  /* "L" is omitted, because we take the current locale into account in Geometry Nodes. */
  /* Allowed type specifiers vary by data type.*/
  if (type.is<std::string>()) {
    pattern += "[s\\?]?";
  }
  else if (type.is<int>()) {
    pattern += "[bBcdoxX]?";
  }
  else if (type.is<float>()) {
    pattern += "[aAeEfFgG]?";
  }
  return {std::regex{pattern}, width_identifier_group, precision_identifier_group};
}

static const FormatPatternInfo *get_pattern_by_type(const CPPType &type)
{
  if (type.is<float>()) {
    static FormatPatternInfo info{get_pattern_by_type_impl(CPPType::get<float>())};
    return &info;
  }
  if (type.is<int>()) {
    static FormatPatternInfo info{get_pattern_by_type_impl(CPPType::get<int>())};
    return &info;
  }
  if (type.is<std::string>()) {
    static FormatPatternInfo info{get_pattern_by_type_impl(CPPType::get<std::string>())};
    return &info;
  }
  return nullptr;
}

static bool format_strings(const StringRef format,
                           const Span<GVArray> inputs,
                           const VectorSet<std::string> &input_names,
                           const IndexMask &mask,
                           MutableSpan<std::string> r_formatted_strings)
{
  CPPType::get<std::string>().value_initialize_indices(r_formatted_strings.data(), mask);

  bool non_auto_index_used = false;
  int64_t next_auto_input_index = 0;

  auto find_input_index = [&](const StringRef identifier) -> std::optional<int64_t> {
    if (identifier.is_empty()) {
      if (non_auto_index_used) {
        /* Once the first explicit identifier is used, it's not allowed to use the auto-index
         * anymore. Only other explicit identifiers are allowed. */
        return std::nullopt;
      }
      if (next_auto_input_index == inputs.size()) {
        return std::nullopt;
      }
      return next_auto_input_index++;
    }
    non_auto_index_used = true;
    if (std::isdigit(identifier[0])) {
      int64_t index;
      std::from_chars_result res = std::from_chars(identifier.begin(), identifier.end(), index);
      if (res.ec != std::errc()) {
        return std::nullopt;
      }
      if (res.ptr < identifier.end()) {
        /* There are other characters after the number.*/
        return std::nullopt;
      }
      if (index >= inputs.size()) {
        return std::nullopt;
      }
      return index;
    }
    const int index = input_names.index_of_try_as(identifier);
    if (index == -1) {
      return std::nullopt;
    }
    return index;
  };

  int64_t current_index = 0;
  while (current_index < format.size()) {
    std::string copy_str;
    const int64_t next_format_start_or_end = find_next_format_start_or_end(
        format, current_index, copy_str);
    if (!copy_str.empty()) {
      mask.foreach_index([&](const int64_t i) {
        std::string &output = r_formatted_strings[i];
        output.append(copy_str);
      });
    }
    if (next_format_start_or_end == format.size()) {
      return true;
    }
    current_index = next_format_start_or_end;

    const std::optional<int64_t> format_length = find_format_length(format.substr(current_index));
    if (!format_length.has_value()) {
      return false;
    }
    const StringRef single_format_with_braces = format.substr(current_index, *format_length);
    const StringRef single_format = single_format_with_braces.substr(
        1, single_format_with_braces.size() - 2);

    StringRef identifier;
    StringRef format_pattern;

    const int64_t colon_index = single_format.find(':');
    if (colon_index == StringRef::not_found) {
      identifier = single_format;
    }
    else {
      identifier = single_format.substr(0, colon_index);
      format_pattern = single_format.substr(colon_index + 1);
    }
    const std::optional<int64_t> input_index = find_input_index(identifier);
    if (!input_index.has_value()) {
      return false;
    }
    const GVArray &input = inputs[*input_index];
    const CPPType &type = input.type();

    const FormatPatternInfo *allowed_pattern = get_pattern_by_type(type);
    if (!allowed_pattern) {
      return false;
    }

    std::string format_str;
    format_str += "{:";
    format_str.append(format_pattern.begin(), format_pattern.end());
    format_str += '}';

    const GVArray *width_input = nullptr;
    const GVArray *precision_input = nullptr;

    Vector<std::string> formats_to_replace;

    std::cmatch m;
    if (std::regex_search(
            format_pattern.begin(), format_pattern.end(), m, allowed_pattern->pattern))
    {
      const std::string width_identifier_with_braces = m.str(
          allowed_pattern->width_identifier_group);
      if (!width_identifier_with_braces.empty()) {
        const StringRef width_identifier =
            StringRef(width_identifier_with_braces).drop_prefix(1).drop_suffix(1);
        const std::optional<int> width_input_index = find_input_index(width_identifier);
        if (!width_input_index.has_value()) {
          return false;
        }
        width_input = &inputs[*width_input_index];
        if (!width_input->type().is<int>()) {
          return false;
        }
        formats_to_replace.append(width_identifier_with_braces);
      }
      if (allowed_pattern->precision_identifier_group.has_value()) {
        const std::string precision_identifier_with_braces = m.str(
            *allowed_pattern->precision_identifier_group);
        if (!precision_identifier_with_braces.empty()) {
          const StringRef precision_identifier =
              StringRef(precision_identifier_with_braces).drop_prefix(1).drop_suffix(1);
          const std::optional<int> precision_input_index = find_input_index(precision_identifier);
          if (!precision_input_index.has_value()) {
            return false;
          }
          precision_input = &inputs[*precision_input_index];
          if (!precision_input->type().is<int>()) {
            return false;
          }
          formats_to_replace.append(precision_identifier_with_braces);
        }
      }
    }

    for (const std::string &old : formats_to_replace) {
      const int64_t old_start = format_str.find(old);
      if (old_start != std::string::npos) {
        format_str.replace(old_start, old.size(), "{}");
      }
    }

    const fmt::format_string<> parsed_format_str{fmt::runtime(format_str)};

    if (std::regex_match(format_pattern.begin(), format_pattern.end(), allowed_pattern->pattern)) {
      const auto append_single_formatted_string = [&](const auto &varray) {
        mask.foreach_index([&](const int64_t i) {
          std::string &output = r_formatted_strings[i];
          auto output_inserter = std::back_inserter(output);
          try {
            if (precision_input) {
              const int precision = std::max(0, precision_input->get<int>(i));
              if (width_input) {
                const int width = std::max(0, width_input->get<int>(i));
                fmt::format_to(output_inserter, parsed_format_str, varray[i], width, precision);
              }
              else {
                fmt::format_to(output_inserter, parsed_format_str, varray[i], precision);
              }
            }
            else {
              if (width_input) {
                const int width = std::max(0, width_input->get<int>(i));
                fmt::format_to(output_inserter, parsed_format_str, varray[i], width);
              }
              else {
                fmt::format_to(output_inserter, parsed_format_str, varray[i]);
              }
            }
          }
          catch (const fmt::format_error &error) {
            /* Invalid patterns should have been caughed before already. */
            BLI_assert_unreachable();
          }
        });
      };

      if (type.is<float>()) {
        append_single_formatted_string(input.typed<float>());
      }
      else if (type.is<int>()) {
        append_single_formatted_string(input.typed<int>());
      }
      else if (type.is<std::string>()) {
        append_single_formatted_string(input.typed<std::string>());
      }
      else {
        return false;
      }
    }
    else {
      return false;
    }
    current_index += *format_length;
  }
  return true;
}

class FormatStringMultiFunction : public mf::MultiFunction {
 private:
  const bNode &node_;
  VectorSet<std::string> input_names_;
  mf::Signature signature_;

 public:
  FormatStringMultiFunction(const bNode &node) : node_(node)
  {
    const NodeFunctionFormatString &storage = node_storage(node);

    mf::SignatureBuilder builder{"Format String", signature_};
    builder.single_input<std::string>("Format");
    for (const int i : IndexRange(storage.items_num)) {
      const NodeFunctionFormatStringItem &item = storage.items[i];
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
      const CPPType &type = *bke::socket_type_to_geo_nodes_base_cpp_type(socket_type);
      builder.single_input(item.name, type);
      input_names_.add_new(StringRef(item.name));
    }

    builder.single_output<std::string>("String");

    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const NodeFunctionFormatString &storage = node_storage(node_);

    const VArray<std::string> formats = params.readonly_single_input<std::string>(0, "Format");
    MutableSpan<std::string> outputs = params.uninitialized_single_output<std::string>(
        storage.items_num + 1, "String");

    Array<GVArray> inputs(storage.items_num);
    for (const int i : IndexRange(storage.items_num)) {
      inputs[i] = params.readonly_single_input(i + 1);
    }

    if (const std::optional<std::string> single_format = formats.get_if_single()) {
      if (!format_strings(*single_format, inputs, input_names_, mask, outputs)) {
        mask.foreach_index([&](const int64_t i) { outputs[i].clear(); });
      }
    }
    else {
      mask.foreach_index(GrainSize(256), [&](const int64_t i) {
        const StringRef format = formats[i];
        if (!format_strings(format, inputs, input_names_, IndexRange::from_single(i), outputs)) {
          outputs[i].clear();
        }
      });
    }
  }
};

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  builder.construct_and_set_matching_fn<FormatStringMultiFunction>(builder.node());
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeFormatString");
  ntype.ui_name = "Format String";
  ntype.ui_description = "Create a string from a format-string and a values to insert";
  ntype.nclass = NODE_CLASS_CONVERTER;
  blender::bke::node_type_storage(
      ntype, "NodeFunctionFormatString", node_free_storage, node_copy_storage);
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.initfunc = node_init;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.insert_link = node_insert_link;
  ntype.register_operators = node_operators;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_format_string_cc

namespace blender::nodes {

StructRNA *FormatStringItemsAccessor::item_srna = &RNA_NodeFunctionFormatStringItem;

void FormatStringItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void FormatStringItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
