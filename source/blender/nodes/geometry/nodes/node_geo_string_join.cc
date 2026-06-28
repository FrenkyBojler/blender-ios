/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_virtual_array.hh"

#include "FN_field.hh"

#include "NOD_geometry_nodes_list.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_string_join_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  auto &delimiter = b.add_input<decl::String>("Delimiter"_ustr).structure_type(StructureType::Dynamic);
  auto &strings = b.add_input<decl::String>("Strings"_ustr).structure_type(StructureType::Dynamic).multi_input().hide_value();
  const std::array<int, 2> input_deps = {delimiter.index(), strings.index()};
  b.add_output<decl::String>("String"_ustr).align_with_previous()
        .inferred_structure_type(input_deps)
        .propagate_references(input_deps);
}

class StringJointFunction : public mf::MultiFunction {
 public:
  StringJointFunction()
  {
    static mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder("join(string...)", signature);
      builder.vector_input<std::string>("Inputs");
      builder.single_output<std::string>("Result");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VVectorArray<std::string> &strings = params.readonly_vector_input<std::string>(0, "Inputs");
    MutableSpan<std::string> result = params.uninitialized_single_output<std::string>(1, "Result");

    const int elements_num = strings.size();

    if (elements_num == 1) {
      mask.foreach_index_optimized<int>([&](const int i) {
        new (&result[i]) std::string (strings.get_vector_element(0, i));
      }, exec_mode::serial);
      return;
    }
    
    mask.foreach_index([&](const int i) {
      std::string buffer;
      for (const int element_i : IndexRange(elements_num)) {
        buffer += strings.get_vector_element(element_i, i);
      }
      new (&result[i]) std::string (std::move(buffer));
    }, exec_mode::serial);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  auto delim = params.extract_input<bke::SocketValueVariant>("Delimiter"_ustr);
  auto strings_list = params.extract_input<bke::SocketValueVariant>("Strings"_ustr);

  std::string error_message;

  const static StringJointFunction func;

  bke::SocketValueVariant output_value;
  if (!execute_multi_function_on_value_variant(
          func,
          {&delim, &strings_list},
          {&output_value},
          params.user_data(),
          error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Value"_ustr, std::move(output_value));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeStringJoin"_ustr, GEO_NODE_STRING_JOIN);
  ntype.ui_name = "Join Strings";
  ntype.ui_description = "Combine any number of input strings";
  ntype.enum_name_legacy = "STRING_JOIN";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_string_join_cc
