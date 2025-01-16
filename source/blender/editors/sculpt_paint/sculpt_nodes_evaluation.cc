/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_brush_types.h"

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

#include "sculpt_nodes_evaluation.hh"

namespace blender::ed::sculpt_paint {

static bool is_socket_type_supported(eNodeSocketDatatype type)
{
  return ELEM(type, SOCK_VECTOR, SOCK_RGBA, SOCK_FLOAT);
}

/**
 * Evaluates the Geometry Nodes node group associated with the specified brush in the given
 * context.
 *
 * The `ExpectedType` should be `float` for all brushes except the Vector Displacement
 * brush, which requires `float3`. The first output socket of the node group is evaluated
 * and used to scale `output_targets`; all other outputs are ignored.
 *
 * Currently supports the following output types: vector, float, and
 * color.
 *
 * TODO: This whole function shouldn't be templated, instead type conversions should be done with
 * fields and only a small amount of code should depend on the result type.
 */
template<typename ExpectedType>
static void sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                  const Object &object,
                                  const Brush &brush,
                                  const bke::SculptFieldContext &context,
                                  const MutableSpan<ExpectedType> output_targets)
{
  const bNodeTree *tree = brush.node_group;

  /* The brush doesn't have an associated node group. */
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

  /* Only consider the first output. The other outputs are not evaluated. */
  const bke::SocketValueVariant output_socket = std::move(
      *param_outputs[0].get<bke::SocketValueVariant>());

  /* Convert the field type to the expected type */
  bke::DataTypeConversions conversions = bke::get_implicit_type_conversions();
  fn::Field<ExpectedType> field_to_evaluate = conversions.try_convert(
      output_socket.get<fn::GField>(), CPPType::get<ExpectedType>());

  Array<ExpectedType> field_outputs(output_targets.size());

  /* Evaluate the field */
  fn::FieldEvaluator evaluator{context, output_targets.size()};
  evaluator.add_with_destination(field_to_evaluate, field_outputs.as_mutable_span());
  evaluator.evaluate();

  /* Scale the output targets */
  for (const int i : output_targets.index_range()) {
    output_targets[i] *= field_outputs[i];
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

template<typename TargetType>
void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                const Object &object,
                                const Brush &brush,
                                const Span<float3> vert_positions,
                                const Span<int> verts,
                                const MutableSpan<TargetType> output_targets)
{
  const Mesh *mesh = static_cast<const Mesh *>(object.data);
  bke::MeshSculptFieldContext context(depsgraph, object, *mesh, verts, vert_positions);

  threading::isolate_task(
      [&]() { sculpt_nodes_evaluate(depsgraph, object, brush, context, output_targets); });
}

template void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                         const Object &object,
                                         const Brush &brush,
                                         const Span<float3> vert_positions,
                                         const Span<int> verts,
                                         const MutableSpan<float> output_targets);

template void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                         const Object &object,
                                         const Brush &brush,
                                         const Span<float3> vert_positions,
                                         const Span<int> verts,
                                         const MutableSpan<float3> output_targets);

template<typename TargetType>
void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const Brush &brush,
                                 const SubdivCCG &subdiv_ccg,
                                 const Span<int> grids,
                                 const Span<float3> positions,
                                 const MutableSpan<TargetType> output_targets)
{
  bke::GridsSculptFieldContext context(depsgraph, object, subdiv_ccg, grids, positions);

  threading::isolate_task(
      [&]() { sculpt_nodes_evaluate(depsgraph, object, brush, context, output_targets); });
}

template void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                          const Object &object,
                                          const Brush &brush,
                                          const SubdivCCG &subdiv_ccg,
                                          const Span<int> grids,
                                          const Span<float3> positions,
                                          const MutableSpan<float> output_targets);

template void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                          const Object &object,
                                          const Brush &brush,
                                          const SubdivCCG &subdiv_ccg,
                                          const Span<int> grids,
                                          const Span<float3> positions,
                                          const MutableSpan<float3> output_targets);

template<typename TargetType>
void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const Brush &brush,
                                 const Set<BMVert *, 0> &verts,
                                 const Span<float3> positions,
                                 const MutableSpan<TargetType> output_targets)
{
  bke::BMeshSculptFieldContext context(depsgraph, object, verts, positions);

  threading::isolate_task(
      [&]() { sculpt_nodes_evaluate(depsgraph, object, brush, context, output_targets); });
}

template void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                          const Object &object,
                                          const Brush &brush,
                                          const Set<BMVert *, 0> &verts,
                                          const Span<float3> positions,
                                          const MutableSpan<float> output_targets);

template void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                          const Object &object,
                                          const Brush &brush,
                                          const Set<BMVert *, 0> &verts,
                                          const Span<float3> positions,
                                          const MutableSpan<float3> output_targets);

}  // namespace blender::ed::sculpt_paint
