/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_string_join_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::String>("Delimiter"_ustr);
  b.add_input<decl::String>("Strings"_ustr).multi_input().hide_value();
  b.add_output<decl::String>("String"_ustr).align_with_previous();
}

class StringJointFunction : public MultiFunction {
 public:
  StringJointFunction()
  {
    static Signature signature = []() {
      Signature signature;
      SignatureBuilder builder("join(string...)", signature);
      builder.vector_input<std::string>("Inputs");
      builder.single_output<std::string>("Result");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context context) const override
  {
    const VVectorArray<std::string> strings = params.readonly_vector_input<std::string>(0, "Inputs");
    MutableSpan<std::string> result = params.uninitialized_single_output<std::string>(1, "Result");

    const int elements_num = strings.size();

    if (elements_num == 1) {
      mask.foreach_index_optimized<int>([&](const int i) {
        new (result[i]) std::string strings.get_vector_element(0, i);
      });
      return;
    }
    
    mask.foreach_index<int>([&](const int i) {
      std::string buffer;
      for (const int element_i : IndexRange(elements_num)) {
        buffer += strings.get_vector_element(element_i, i);
      }
      new result[i] std::string std::move(buffer);
    });
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  auto strings = params.extract_input<GListPtr>("Strings"_ustr);
  const std::string delim = params.extract_input<std::string>("Delimiter"_ustr);

  if (index_value_variant.is_single()) {
    /* Optimization for the case when the index is a single value. Here only that one index has to
     * be evaluated. */
    const int domain_size = component->attribute_domain_size(domain);
    int index = index_value_variant.extract<int>();
    if (use_clamp) {
      index = std::clamp(index, 0, domain_size - 1);
    }
    const eNodeSocketDatatype socket_type = params.node().output_socket(0).typeinfo->type;
    SocketValueVariant output_value;
    void *buffer = output_value.allocate_single(socket_type);
    if (index >= 0 && index < domain_size) {
      const IndexMask mask = IndexRange(index, 1);
      const bke::GeometryFieldContext geometry_context(*component, domain);
      FieldEvaluator evaluator(geometry_context, &mask);
      evaluator.add(value_field);
      evaluator.evaluate();
      const GVArray &data = evaluator.get_evaluated(0);
      data.get_to_uninitialized(index, buffer);
    }
    else {
      cpp_type.copy_construct(cpp_type.default_value(), buffer);
    }
    params.set_output("Value"_ustr, std::move(output_value));
    return;
  }

  std::string error_message;

  if (use_clamp) {
    bke::SocketValueVariant index_value_variant_copy = index_value_variant;
    static auto clamp_fn = mf::build::SI3_SO<int, int, int, int>(
        "Clamp",
        [](int value, int min, int max) { return std::clamp(value, min, max); },
        mf::build::exec_presets::SomeSpanOrSingle<0>());
    const int domain_size = component->attribute_domain_size(domain);
    bke::SocketValueVariant min_value = bke::SocketValueVariant::From(0);
    bke::SocketValueVariant max_value = bke::SocketValueVariant::From(domain_size - 1);
    if (!execute_multi_function_on_value_variant(
            clamp_fn,
            {&index_value_variant_copy, &min_value, &max_value},
            {&index_value_variant},
            params.user_data(),
            error_message))
    {
      params.set_default_remaining_outputs();
      params.error_message_add(NodeWarningType::Error, std::move(error_message));
      return;
    }
  }

  bke::SocketValueVariant output_value;
  if (!execute_multi_function_on_value_variant(
          std::make_shared<bke::SampleIndexFunction>(
              std::move(geometry), std::move(value_field), domain),
          {&index_value_variant},
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
