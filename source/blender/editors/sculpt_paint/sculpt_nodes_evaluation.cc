/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_brush_types.h"

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket.hh"

#include "BKE_brush.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idprop.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "FN_field.hh"
#include "FN_lazy_function_execute.hh"

#include "editors/sculpt_paint/sculpt_intern.hh"
#include "editors/sculpt_paint/sculpt_nodes_evaluation.hh"

namespace blender::ed::sculpt_paint {

static void sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                  Object &object,
                                  const Brush &brush,
                                  StrokeCache &cache,
                                  bke::SculptFieldContext &context,
                                  MutableSpan<float3> outputs)
{
  //const bNodeTree* tree = brush.node_tree; 
  const bNodeTree* tree = cache.node_tree;

  if (tree == nullptr) {
    return;
  }

  const nodes::GeometryNodesLazyFunctionGraphInfo &lf_graph_info =
      *nodes::ensure_geometry_nodes_lazy_function_graph(*tree);
  const nodes::GeometryNodesGroupFunction &function = lf_graph_info.function;
  const lf::LazyFunction &lazy_function = *function.function;
  const int num_inputs = lazy_function.inputs().size();
  const int num_outputs = lazy_function.outputs().size();

  /* Nothing to do */
  if (num_outputs == 0) {
    outputs.fill(float3(0.0f));
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
  sculpt_data.plane_normal = cache.sculpt_normal_symm;
  sculpt_data.plane_center = cache.sculpt_center_symm;
  sculpt_data.cursor_location = cache.location_symm;
  sculpt_data.pen_pressure = cache.pressure;
  sculpt_data.radius = cache.radius;
  sculpt_data.strength = brush.alpha;
  sculpt_data.is_first_step = cache.first_time;
  sculpt_data.step = cache.step;
  sculpt_data.color = cache.paint_brush.color;
  sculpt_data.local_transform = calc_local_space_matrix(cache, cache.sculpt_center_symm);
  sculpt_data.texture_transform = calc_texture_space_matrix(cache);
  sculpt_data.depsgraph = &depsgraph;
  sculpt_data.self_object = &object;

  nodes::GeoNodesCallData call_data;
  call_data.root_ntree = tree;
  call_data.side_effect_nodes = {};
  call_data.sculpt_data = &sculpt_data;

  bke::SculptingComputeContext compute_context;

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
    const eNodeSocketDatatype socket_type = typeinfo ? eNodeSocketDatatype(typeinfo->type) :
                                                       SOCK_CUSTOM;

    const CPPType *type = typeinfo->geometry_nodes_cpp_type;
    BLI_assert(type != nullptr);
    void *value = allocator.allocate(type->size(), type->alignment());
    // initialize_group_input(btree, properties, i, value);
    param_inputs[function.inputs.main[i]] = {type, value};
    inputs_to_destruct.append({type, value});
  }

  /* Prepare used-outputs inputs. */
  Array<bool> output_used_inputs(tree->interface_outputs().size(), true);
  for (const int i : tree->interface_outputs().index_range()) {
    param_inputs[function.inputs.output_usages[i]] = &output_used_inputs[i];
  }

  /* No anonymous attributes have to be propagated. */
  /*Array<bke::GeometryNodesReferenceSet> references_to_propagate(
    function.inputs.references_to_propagate.geometry_outputs.size());
  for (const int i : references_to_propagate.index_range()) {
    param_inputs[function.inputs.references_to_propagate.range[i]] = &references_to_propagate[i];
  }*/

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

  for (GMutablePointer &ptr : inputs_to_destruct) {
    ptr.destruct();
  }

  bke::SocketValueVariant output = std::move(*param_outputs[0].get<bke::SocketValueVariant>());

  fn::Field<float3> output_field = output.get<fn::Field<float3>>();
  fn::FieldEvaluator evaluator{context, outputs.size()};

  Vector<float3> tmp_outputs(outputs.size());
  evaluator.add_with_destination(output_field, tmp_outputs.as_mutable_span());
  evaluator.evaluate();

  for (const int i : outputs.index_range()) {
    outputs[i] *= tmp_outputs[i];
  }

  for (const int i : param_outputs.index_range()) {
    if (param_set_outputs[i]) {
      GMutablePointer &ptr = param_outputs[i];
      ptr.destruct();
    }
  }
}

void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                Object &object,
                                const Brush& brush,
                                StrokeCache &cache,
                                const Span<float3> vert_positions,
                                const Span<int> verts,
                                MutableSpan<float3> translations)
{
  Array<float3> positions(verts.size());

  for (const int i : positions.index_range()) {
    positions[i] = vert_positions[verts[i]];
  }

  const Mesh *mesh = static_cast<const Mesh *>(object.data);
  bke::MeshSculptFieldContext context(depsgraph, object, *mesh, positions, verts, {});

  sculpt_nodes_evaluate(depsgraph, object, brush, cache, context, translations);
}

void paint_sculpt_nodes_evaluate(const Depsgraph& depsgraph,
  Object& object,
  const Brush& brush,
  StrokeCache& cache,
  Span<float3> vert_positions,
  Span<int> verts,
  MutableSpan<float4> brush_colors,
  MutableSpan<float4> current_colors)
{
  Vector<float3> positions(verts.size());

  for (const int i : positions.index_range()) {
    positions[i] = vert_positions[verts[i]];
  }

  const Mesh* mesh = static_cast<const Mesh*>(object.data);
  bke::MeshSculptFieldContext context(depsgraph, object, *mesh, positions, verts, current_colors);

  Vector<float3> outputs(verts.size());
  outputs.fill(float3(1.0f));

  sculpt_nodes_evaluate(depsgraph, object, brush, cache, context, outputs);

  for (const int i : brush_colors.index_range()) {
    brush_colors[i] = float4(outputs[i], brush_colors[i].w);
  }
}

void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Brush& brush,
                                 StrokeCache &cache,
                                 SubdivCCG &subdiv_ccg,
                                 Span<int> grids,
                                 Span<float3> positions,
                                 MutableSpan<float3> translations)
{
  bke::GridsSculptFieldContext context(depsgraph, object, subdiv_ccg, grids, positions);
  sculpt_nodes_evaluate(depsgraph, object, brush, cache, context, translations);
}

void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 const Brush &brush,
                                 StrokeCache &cache,
                                 const Set<BMVert *, 0> &verts,
                                 Span<float3> positions,
                                 MutableSpan<float3> translations)
{
  bke::BMeshSculptFieldContext context(depsgraph, object, verts, positions);
  sculpt_nodes_evaluate(depsgraph, object, brush, cache, context, translations);
}

}  // namespace blender::ed::sculpt_paint
