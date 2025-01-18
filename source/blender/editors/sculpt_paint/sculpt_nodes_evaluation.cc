/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <memory>
#include <variant>

#include "BLI_generic_virtual_array.hh"
#include "DNA_brush_types.h"

#include "FN_multi_function_builder.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"
#include "BKE_type_conversions.hh"

#include "FN_field.hh"
#include "FN_lazy_function_execute.hh"

#include "sculpt_intern.hh"
#include "sculpt_nodes_evaluation.hh"

namespace blender::ed::sculpt_paint {

static bool is_socket_type_supported(const eNodeSocketDatatype type)
{
  return ELEM(type, SOCK_VECTOR, SOCK_RGBA, SOCK_FLOAT);
}

template<typename T> class SpanFieldInput final : public fn::FieldInput {
  Span<T> data_;

 public:
  SpanFieldInput(Span<T> data) : FieldInput(CPPType::get<T>(), "Span"), data_(data) {}

  GVArray get_varray_for_context(const fn::FieldContext & /*context*/,
                                 const IndexMask & /*mask*/,
                                 ResourceScope & /*scope*/) const final
  {
    return VArray<T>::ForSpan(data_);
  }
};

struct CombineFactors {
  MutableSpan<float> factors;
};

struct OutputTranslations {
  MutableSpan<float3> translations;
};

using EvaluationResult = std::variant<CombineFactors, OutputTranslations>;

/**
 * Evaluates the Geometry Nodes node group associated with the specified brush in the given
 * context.
 */
static void sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                  const Object &object,
                                  const StrokeCache &cache,
                                  const Brush &brush,
                                  const bke::SculptFieldContext &context,
                                  const EvaluationResult &output)
{
  const bNodeTree *tree = brush.node_group;
  if (tree == nullptr) {
    return;
  }

  const nodes::GeometryNodesLazyFunctionGraphInfo &lf_graph_info =
      *nodes::ensure_geometry_nodes_lazy_function_graph(*tree);
  const nodes::GeometryNodesGroupFunction &function = lf_graph_info.function;
  const lf::LazyFunction &lazy_function = *function.function;
  const int num_inputs = lazy_function.inputs().size();
  const int num_outputs = lazy_function.outputs().size();

  /* Nothing to do. */
  if (num_outputs == 0) {
    return;
  }

  const bNodeTreeInterfaceSocket *first_output = tree->interface_outputs()[0];
  const eNodeSocketDatatype type = (eNodeSocketDatatype)first_output->socket_typeinfo()->type;

  /* Output type is unsupported. TODO: warn user? */
  if (!is_socket_type_supported(type)) {
    return;
  }

  Array<GMutablePointer> param_inputs(num_inputs);
  Array<std::optional<lf::ValueUsage>> param_input_usages(num_inputs);
  Array<lf::ValueUsage> param_output_usages(num_outputs);

  Array<GMutablePointer> param_outputs(num_outputs);
  Array<bool> param_set_outputs(num_outputs, false);

  /* We want to evaluate the main outputs, but don't care about which inputs are used for now. */
  param_output_usages.as_mutable_span().slice(function.outputs.main).fill(lf::ValueUsage::Used);
  param_output_usages.as_mutable_span()
      .slice(function.outputs.input_usages)
      .fill(lf::ValueUsage::Unused);

  nodes::GeoNodesSculptData sculpt_data;
  sculpt_data.depsgraph = &depsgraph;
  sculpt_data.self_object = &object;

  if (cache.vc->rv3d) {
    sculpt_data.view_matrix = float4x4(cache.vc->rv3d->viewmat);
    sculpt_data.projection_matrix = float4x4(cache.vc->rv3d->winmat);
    sculpt_data.is_orthographic = !bool(cache.vc->rv3d->is_persp);
  }
  else {
    sculpt_data.view_matrix = float4x4::identity();
    sculpt_data.projection_matrix = float4x4::identity();
    sculpt_data.is_orthographic = false;
  }

  nodes::GeoNodesCallData call_data;
  call_data.root_ntree = tree;
  call_data.side_effect_nodes = {};
  call_data.sculpt_data = &sculpt_data;

  bke::SculptComputeContext compute_context;

  nodes::GeoNodesLFUserData user_data;
  user_data.call_data = &call_data;
  user_data.compute_context = &compute_context;

  LinearAllocator<> allocator;
  Vector<GMutablePointer> inputs_to_destruct;

  tree->ensure_interface_cache();

  /* Prepare main inputs. */
  for (const int i : tree->interface_inputs().index_range()) {
    const bNodeTreeInterfaceSocket &interface_socket = *tree->interface_inputs()[i];
    const bke::bNodeSocketType *typeinfo = interface_socket.socket_typeinfo();

    const CPPType *type = typeinfo->geometry_nodes_cpp_type;
    BLI_assert(type != nullptr);
    void *value = allocator.allocate(type->size(), type->alignment());

    /* Initialiaze with default values, Group Input is not supported for now. */
    typeinfo->get_geometry_nodes_cpp_value(interface_socket.socket_data, value);

    param_inputs[function.inputs.main[i]] = {type, value};
    inputs_to_destruct.append({type, value});
  }

  /* Prepare used-outputs inputs. */
  Array<bool> output_used_inputs(tree->interface_outputs().size(), true);
  for (const int i : tree->interface_outputs().index_range()) {
    param_inputs[function.inputs.output_usages[i]] = &output_used_inputs[i];
  }

  /* Prepare memory for output values. */
  for (const int i : IndexRange(num_outputs)) {
    const lf::Output &lf_output = lazy_function.outputs()[i];
    const CPPType &type = *lf_output.type;
    void *buffer = allocator.allocate(type.size(), type.alignment());
    param_outputs[i] = {type, buffer};
  }

  nodes::GeoNodesLFLocalUserData local_user_data(user_data);

  lf::Context lf_context(lazy_function.init_storage(allocator), &user_data, &local_user_data);
  lf::BasicParams lf_params{lazy_function,
                            param_inputs,
                            param_outputs,
                            param_input_usages,
                            param_output_usages,
                            param_set_outputs};
  {
    lazy_function.execute(lf_params, lf_context);
  }
  lazy_function.destruct_storage(lf_context.storage);

  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();

  /* Only consider the first output. The other outputs are not evaluated. */
  bke::SocketValueVariant *output_socket = param_outputs[0].get<bke::SocketValueVariant>();
  fn::GField output_field = output_socket->extract<fn::GField>();
  if (const CombineFactors *result = std::get_if<CombineFactors>(&output)) {
    fn::Field<float> converted = conversions.try_convert(std::move(output_field),
                                                         CPPType::get<float>());
    static auto multiply_fn = mf::build::SI2_SO<float, float, float>(
        "Multiply",
        [](float a, float b) { return a * b; },
        mf::build::exec_presets::AllSpanOrSingle());

    fn::Field<float> input_factors(std::make_shared<SpanFieldInput<float>>(result->factors));
    fn::Field<float> final_factor{
        fn::FieldOperation::Create(multiply_fn, {std::move(converted), std::move(input_factors)})};
    fn::FieldEvaluator evaluator{context, result->factors.size()};
    evaluator.add_with_destination(std::move(final_factor), result->factors);
    evaluator.evaluate();
  }
  else if (const OutputTranslations *result = std::get_if<OutputTranslations>(&output)) {
    fn::Field<float3> converted = conversions.try_convert(std::move(output_field),
                                                          CPPType::get<float3>());
    fn::FieldEvaluator evaluator{context, result->translations.size()};
    evaluator.add_with_destination(std::move(converted), result->translations);
    evaluator.evaluate();
  }

  /* Destruct inputs and outputs */
  for (GMutablePointer &ptr : inputs_to_destruct) {
    ptr.destruct();
  }

  for (const int i : param_outputs.index_range()) {
    if (param_set_outputs[i]) {
      GMutablePointer &ptr = param_outputs[i];
      ptr.destruct();
    }
  }
}

void nodes_evaluate_factors_mesh(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const StrokeCache &cache,
                                 const Brush &brush,
                                 const Span<float3> vert_positions,
                                 const Span<int> verts,
                                 const MutableSpan<float> factors)
{
  const Mesh *mesh = static_cast<const Mesh *>(object.data);
  const bke::MeshSculptFieldContext context(depsgraph, object, *mesh, verts, vert_positions);
  threading::isolate_task([&]() {
    const CombineFactors output{factors};
    sculpt_nodes_evaluate(depsgraph, object, cache, brush, context, output);
  });
}

void nodes_evaluate_factors_grids(const Depsgraph &depsgraph,
                                  const Object &object,
                                  const StrokeCache &cache,
                                  const Brush &brush,
                                  const SubdivCCG &subdiv_ccg,
                                  const Span<int> grids,
                                  const Span<float3> positions,
                                  const MutableSpan<float> factors)
{
  const bke::GridsSculptFieldContext context(depsgraph, object, subdiv_ccg, grids, positions);
  threading::isolate_task([&]() {
    const CombineFactors output{factors};
    sculpt_nodes_evaluate(depsgraph, object, cache, brush, context, output);
  });
}

void nodes_evaluate_factors_bmesh(const Depsgraph &depsgraph,
                                  const Object &object,
                                  const StrokeCache &cache,
                                  const Brush &brush,
                                  const Set<BMVert *, 0> &verts,
                                  const Span<float3> positions,
                                  const MutableSpan<float> factors)
{
  const bke::BMeshSculptFieldContext context(depsgraph, object, verts, positions);
  threading::isolate_task([&]() {
    const CombineFactors output{factors};
    sculpt_nodes_evaluate(depsgraph, object, cache, brush, context, output);
  });
}

void nodes_evaluate_translations_mesh(const Depsgraph &depsgraph,
                                      const Object &object,
                                      const StrokeCache &cache,
                                      const Brush &brush,
                                      const Span<float3> vert_positions,
                                      const Span<int> verts,
                                      const MutableSpan<float3> translations)
{
  const Mesh *mesh = static_cast<const Mesh *>(object.data);
  const bke::MeshSculptFieldContext context(depsgraph, object, *mesh, verts, vert_positions);
  threading::isolate_task([&]() {
    const OutputTranslations output{translations};
    sculpt_nodes_evaluate(depsgraph, object, cache, brush, context, output);
  });
}

void nodes_evaluate_translations_grids(const Depsgraph &depsgraph,
                                       const Object &object,
                                       const StrokeCache &cache,
                                       const Brush &brush,
                                       const SubdivCCG &subdiv_ccg,
                                       const Span<int> grids,
                                       const Span<float3> positions,
                                       const MutableSpan<float3> translations)
{
  const bke::GridsSculptFieldContext context(depsgraph, object, subdiv_ccg, grids, positions);
  threading::isolate_task([&]() {
    const OutputTranslations output{translations};
    sculpt_nodes_evaluate(depsgraph, object, cache, brush, context, output);
  });
}

void nodes_evaluate_translations_bmesh(const Depsgraph &depsgraph,
                                       const Object &object,
                                       const StrokeCache &cache,
                                       const Brush &brush,
                                       const Set<BMVert *, 0> &verts,
                                       const Span<float3> positions,
                                       const MutableSpan<float3> translations)
{
  const bke::BMeshSculptFieldContext context(depsgraph, object, verts, positions);
  threading::isolate_task([&]() {
    const OutputTranslations output{translations};
    sculpt_nodes_evaluate(depsgraph, object, cache, brush, context, output);
  });
}

}  // namespace blender::ed::sculpt_paint
