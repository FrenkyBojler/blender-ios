/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"

#include "NOD_geometry_nodes_closure.hh"
#include "NOD_geometry_nodes_lazy_function.hh"

namespace blender::nodes {

std::optional<int> ClosureSignature::find_input_index(const SocketInterfaceKey &key) const
{
  for (const int i : this->inputs.index_range()) {
    const Item &item = this->inputs[i];
    if (item.key.matches(key)) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<int> ClosureSignature::find_output_index(const SocketInterfaceKey &key) const
{
  for (const int i : this->outputs.index_range()) {
    const Item &item = this->outputs[i];
    if (item.key.matches(key)) {
      return i;
    }
  }
  return std::nullopt;
}

static bool items_equal(const ClosureSignature::Item &a, const ClosureSignature::Item &b)
{
  if (!a.key.matches_exactly(b.key)) {
    return false;
  }
  if (a.type != b.type) {
    return false;
  }
  if (a.structure_type.has_value() && b.structure_type.has_value()) {
    if (*a.structure_type != *b.structure_type) {
      return false;
    }
  }
  return true;
}

bool ClosureSignature::matches_exactly(const ClosureSignature &other) const
{
  if (inputs.size() != other.inputs.size()) {
    return false;
  }
  if (outputs.size() != other.outputs.size()) {
    return false;
  }
  for (const Item &item : inputs) {
    if (std::none_of(other.inputs.begin(), other.inputs.end(), [&](const Item &other_item) {
          return items_equal(item, other_item);
        }))
    {
      return false;
    }
  }
  for (const Item &item : outputs) {
    if (std::none_of(other.outputs.begin(), other.outputs.end(), [&](const Item &other_item) {
          return items_equal(item, other_item);
        }))
    {
      return false;
    }
  }
  return true;
}

bool ClosureSignature::all_matching_exactly(const Span<ClosureSignature> signatures)
{
  if (signatures.is_empty()) {
    return true;
  }
  for (const ClosureSignature &signature : signatures.drop_front(1)) {
    if (!signatures[0].matches_exactly(signature)) {
      return false;
    }
  }
  return true;
}

ClosureSignature ClosureSignature::FromClosureOutputNode(const bNode &node)
{
  BLI_assert(node.is_type("GeometryNodeClosureOutput"));
  const auto &storage = *static_cast<const NodeGeometryClosureOutput *>(node.storage);
  nodes::ClosureSignature signature;
  for (const int i : IndexRange(storage.input_items.items_num)) {
    const NodeGeometryClosureInputItem &item = storage.input_items.items[i];
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      signature.inputs.append({nodes::SocketInterfaceKey(item.name), stype});
    }
  }
  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeGeometryClosureOutputItem &item = storage.output_items.items[i];
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      signature.outputs.append({nodes::SocketInterfaceKey(item.name), stype});
    }
  }
  return signature;
}

ClosureSignature ClosureSignature::FromEvaluateClosureNode(const bNode &node)
{
  BLI_assert(node.is_type("GeometryNodeEvaluateClosure"));
  const auto &storage = *static_cast<const NodeGeometryEvaluateClosure *>(node.storage);
  nodes::ClosureSignature signature;
  for (const int i : IndexRange(storage.input_items.items_num)) {
    const NodeGeometryEvaluateClosureInputItem &item = storage.input_items.items[i];
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      signature.inputs.append({nodes::SocketInterfaceKey(item.name),
                               stype,
                               nodes::StructureType(item.structure_type)});
    }
  }
  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeGeometryEvaluateClosureOutputItem &item = storage.output_items.items[i];
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      signature.outputs.append({nodes::SocketInterfaceKey(item.name),
                                stype,
                                nodes::StructureType(item.structure_type)});
    }
  }
  return signature;
}

std::shared_ptr<ClosureSignature> ClosureSignature::FromBuiltin(const ClosureSocketValueType type)
{
  switch (type) {
    case CLOSURE_SOCKET_VALUE_TYPE_NONE: {
      return {};
    }
    case CLOSURE_SOCKET_VALUE_TYPE_FLOAT_CURVE: {
      static std::shared_ptr<nodes::ClosureSignature> signature = []() {
        std::shared_ptr<nodes::ClosureSignature> signature =
            std::make_shared<nodes::ClosureSignature>();
        const bke::bNodeSocketType *float_socket_type = bke::node_socket_type_find_static(
            SOCK_FLOAT);
        signature->inputs.append({nodes::SocketInterfaceKey("Value"), float_socket_type});
        signature->outputs.append({nodes::SocketInterfaceKey("Value"), float_socket_type});
        return signature;
      }();
      return signature;
    }
    case CLOSURE_SOCKET_VALUE_TYPE_VECTOR_CURVE: {
      static std::shared_ptr<nodes::ClosureSignature> signature = []() {
        std::shared_ptr<nodes::ClosureSignature> signature =
            std::make_shared<nodes::ClosureSignature>();
        const bke::bNodeSocketType *vector_socket_type = bke::node_socket_type_find_static(
            SOCK_VECTOR);
        signature->inputs.append({nodes::SocketInterfaceKey("Value"), vector_socket_type});
        signature->outputs.append({nodes::SocketInterfaceKey("Value"), vector_socket_type});
        return signature;
      }();
      return signature;
    }
    case CLOSURE_SOCKET_VALUE_TYPE_COLOR_CURVE: {
      static std::shared_ptr<nodes::ClosureSignature> signature = []() {
        std::shared_ptr<nodes::ClosureSignature> signature =
            std::make_shared<nodes::ClosureSignature>();
        const bke::bNodeSocketType *color_socket_type = bke::node_socket_type_find_static(
            SOCK_RGBA);
        signature->inputs.append({nodes::SocketInterfaceKey("Value"), color_socket_type});
        signature->outputs.append({nodes::SocketInterfaceKey("Value"), color_socket_type});
        return signature;
      }();
      return signature;
    }
    case CLOSURE_SOCKET_VALUE_TYPE_COLOR_RAMP: {
      static std::shared_ptr<nodes::ClosureSignature> signature = []() {
        std::shared_ptr<nodes::ClosureSignature> signature =
            std::make_shared<nodes::ClosureSignature>();
        const bke::bNodeSocketType *float_socket_type = bke::node_socket_type_find_static(
            SOCK_FLOAT);
        const bke::bNodeSocketType *color_socket_type = bke::node_socket_type_find_static(
            SOCK_RGBA);
        signature->inputs.append({nodes::SocketInterfaceKey("Value"), float_socket_type});
        signature->outputs.append({nodes::SocketInterfaceKey("Value"), color_socket_type});
        return signature;
      }();
      return signature;
    }
  }
  return {};
}

class ClosureLazyFunctionForMultiFunction : public lf::LazyFunction {
 private:
  const ClosureSignature &closure_signature_;
  std::shared_ptr<mf::MultiFunction> multi_function_;
  ClosureFunctionIndices indices_;

 public:
  ClosureLazyFunctionForMultiFunction(const ClosureSignature &closure_signature,
                                      std::shared_ptr<mf::MultiFunction> multi_function)
      : closure_signature_(closure_signature), multi_function_(multi_function)
  {
    debug_name_ = "Closure Multi Function";
    /* Add main inputs and outputs. */
    for (const int param_i : multi_function->param_indices()) {
      const mf::ParamType param_type = multi_function->param_type(param_i);
      const StringRefNull param_name = multi_function->param_name(param_i);
      switch (param_type.category()) {
        case mf::ParamCategory::SingleInput: {
          inputs_.append_as(
              param_name.c_str(), CPPType::get<bke::SocketValueVariant>(), lf::ValueUsage::Used);
          break;
        }
        case mf::ParamCategory::SingleOutput: {
          outputs_.append_as(param_name.c_str(), CPPType::get<bke::SocketValueVariant>());
          break;
        }
        default: {
          /* Not supported yet. */
          BLI_assert_unreachable();
          break;
        }
      }
    }
    indices_.inputs.main = inputs_.index_range();
    indices_.outputs.main = outputs_.index_range();

    /* Add output usage inputs.*/
    for (const int param_i : multi_function->param_indices()) {
      const mf::ParamType param_type = multi_function->param_type(param_i);
      if (param_type.category() != mf::ParamCategory::SingleOutput) {
        continue;
      }
      inputs_.append_as("Usage", CPPType::get<bool>(), lf::ValueUsage::Unused);
    }
    indices_.inputs.output_usages = inputs_.index_range().drop_front(indices_.inputs.main.size());

    /* Add input usage outputs.*/
    for (const int param_i : multi_function->param_indices()) {
      const mf::ParamType param_type = multi_function->param_type(param_i);
      if (param_type.category() != mf::ParamCategory::SingleInput) {
        continue;
      }
      outputs_.append_as("Usage", CPPType::get<bool>());
    }
    indices_.outputs.input_usages = outputs_.index_range().drop_front(
        indices_.outputs.main.size());
  }

  ClosureFunctionIndices indices() const
  {
    return indices_;
  }

  void execute_impl(lf::Params &params, const lf::Context & /*context*/) const override
  {
    Vector<bke::SocketValueVariant *> input_values;
    Vector<bke::SocketValueVariant *> output_values;

    for (const int i : indices_.outputs.input_usages) {
      params.set_output(i, true);
    }

    for (const int i : indices_.inputs.main) {
      input_values.append(&params.get_input<bke::SocketValueVariant>(i));
    }
    for (const int i : indices_.outputs.main) {
      output_values.append(new (params.get_output_data_ptr(i)) bke::SocketValueVariant());
    }

    std::string error_message;
    if (!execute_multi_function_on_value_variant(*multi_function_,
                                                 multi_function_,
                                                 input_values,
                                                 output_values,
                                                 nullptr,
                                                 error_message))
    {
      for (const int i : indices_.outputs.main.index_range()) {
        std::destroy_at(output_values[i]);
        construct_socket_default_value(*closure_signature_.outputs[i].type, output_values[i]);
      }
    }

    for (const int i : indices_.outputs.main) {
      params.output_set(i);
    }
  }
};

ClosurePtr Closure::FromMultiFunction(std::shared_ptr<ClosureSignature> signature,
                                      std::shared_ptr<mf::MultiFunction> multi_function,
                                      Vector<const void *> default_input_values,
                                      std::optional<ClosureSourceLocation> source_location,
                                      std::shared_ptr<ClosureEvalLog> eval_log)
{
  std::unique_ptr<ResourceScope> scope = std::make_unique<ResourceScope>();
  const auto &lazy_function = scope->construct<ClosureLazyFunctionForMultiFunction>(
      *signature, std::move(multi_function));
  return ClosurePtr(MEM_new<Closure>(__func__,
                                     std::move(signature),
                                     std::move(scope),
                                     lazy_function,
                                     lazy_function.indices(),
                                     default_input_values,
                                     std::move(source_location),
                                     std::move(eval_log)));
}

}  // namespace blender::nodes
