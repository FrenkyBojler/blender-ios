/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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

static bool format_strings(const StringRef format,
                           const Span<GVArray> inputs,
                           const IndexMask &mask,
                           MutableSpan<std::string> r_formatted_strings)
{
  mask.foreach_index([&](const int64_t i) {
    std::string *output = &r_formatted_strings[i];
    new (output) std::string();
  });

  static std::regex simple_number_pattern(R"#((\.\d+)?)#");

  int64_t next_auto_input_index = 0;

  int64_t current_index = 0;
  while (current_index < format.size()) {
    const int64_t next_format_start = format.find('{', current_index);
    const int64_t copy_length = next_format_start == StringRef::not_found ?
                                    format.size() - current_index :
                                    next_format_start - current_index;
    if (copy_length > 0) {
      const StringRef str_to_copy = format.substr(current_index, copy_length);
      mask.foreach_index([&](const int64_t i) {
        r_formatted_strings[i].append(str_to_copy.data(), str_to_copy.size());
      });
    }
    if (next_format_start == StringRef::not_found) {
      break;
    }
    current_index = next_format_start;
    const std::optional<int64_t> format_length = find_format_length(format.substr(current_index));
    if (!format_length.has_value()) {
      /* TODO: How to handle this case? */
      return false;
    }
    const StringRef single_format_with_braces = format.substr(current_index, *format_length);
    const StringRef single_format = single_format_with_braces.substr(
        1, single_format_with_braces.size() - 2);

    StringRef identifier;
    StringRef format_pattern;

    const int64_t colon_index = single_format.find(':');
    if (colon_index == StringRef::not_found) {
      format_pattern = single_format;
    }
    else {
      identifier = single_format.substr(0, colon_index);
      format_pattern = single_format.substr(colon_index + 1);
    }

    if (std::regex_match(format_pattern.begin(), format_pattern.end(), simple_number_pattern)) {
      const int64_t input_index = next_auto_input_index++;
      if (input_index >= inputs.size()) {
        return false;
      }
      std::string format_str;
      format_str += "{:";
      format_str.append(format_pattern.begin(), format_pattern.end());
      format_str += '}';

      const GVArray &input = inputs[input_index];
      const CPPType &type = input.type();

      const auto append_single_formatted_string = [&](const auto &varray) {
        mask.foreach_index([&](const int64_t i) {
          std::string &output = r_formatted_strings[i];
          fmt::format_to(std::back_inserter(output), fmt::runtime(format_str), varray[i]);
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
    current_index += *format_length;
  }
  return true;
}

class FormatStringMultiFunction : public mf::MultiFunction {
 private:
  const bNode &node_;
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
      format_strings(*single_format, inputs, mask, outputs);
    }
    else {
      mask.foreach_index(GrainSize(256), [&](const int64_t i) {
        const StringRef format = formats[i];
        format_strings(format, inputs, IndexRange::from_single(i), outputs);
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

  fn_node_type_base(&ntype, "FunctionNodeFormatString", FN_NODE_FORMAT_STRING);
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
int FormatStringItemsAccessor::node_type = FN_NODE_FORMAT_STRING;

void FormatStringItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void FormatStringItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
