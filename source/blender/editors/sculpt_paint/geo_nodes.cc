/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket.hh"

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

namespace blender::ed::sculpt_paint {

/* TODO: move to more appropriate file */
static float4x4& calc_local_space_matrix(StrokeCache& cache,
  const float3& plane_normal,
  const float3& plane_center)
{
  float4x4 mat = float4x4::identity();
  mat.x_axis() = math::cross(plane_normal, cache.grab_delta_symm);
  mat.y_axis() = math::cross(plane_normal, float3(mat[0]));
  mat.z_axis() = plane_normal;
  mat.location() = plane_center;
  mat = math::normalize(mat);

  float4x4 scale = math::from_scale<float4x4>(float3(cache.radius));
  float4x4 scaled_mat = mat * scale;
  float4x4 inv_mat = math::invert(scaled_mat);

  return inv_mat;
}

/* TODO: move to more appropriate file */
static float4x4& calc_texture_space_matrix(StrokeCache& cache)
{
  float4x4 mat = math::from_location<float4x4>(float3(0.5f, 0.5f, 0.0f));
  mat *= math::from_scale<float4x4>(float3(0.5f, 0.5f, 1.0f));
  mat *= cache.brush_local_mat;

  float4x4 mirror_symmetry_mat = float4x4::identity();

  if (cache.mirror_symmetry_pass & PAINT_SYMM_X) {
    mirror_symmetry_mat[0][0] = -1;
  }

  if (cache.mirror_symmetry_pass & PAINT_SYMM_Y) {
    mirror_symmetry_mat[1][1] = -1;
  }

  if (cache.mirror_symmetry_pass & PAINT_SYMM_Z) {
    mirror_symmetry_mat[2][2] = -1;
  }

  mat *= mirror_symmetry_mat;

  if (cache.radial_symmetry_pass) {
    mat *= cache.symm_rot_mat_inv;
  }

  return mat;
}

template <typename T>
static void sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                  Object &object,
                                  StrokeCache &cache,
                                  bke::SculptFieldContext &context,
                                  MutableSpan<T> outputs)
{
  const bNodeTree &tree = *cache.node_tree;

  const nodes::GeometryNodesLazyFunctionGraphInfo &lf_graph_info =
      *nodes::ensure_geometry_nodes_lazy_function_graph(tree);
  const nodes::GeometryNodesGroupFunction &function = lf_graph_info.function;
  const lf::LazyFunction &lazy_function = *function.function;
  const int num_inputs = lazy_function.inputs().size();
  const int num_outputs = lazy_function.outputs().size();

  /* Nothing to do */
  if (num_outputs == 0) {
    outputs.fill(T(0.0f));
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
  sculpt_data.plane_center = cache.location_symm;
  sculpt_data.cursor_location = cache.location_symm;
  sculpt_data.pen_pressure = cache.pressure;
  sculpt_data.radius = cache.radius;
  sculpt_data.strength = cache.bstrength;
  sculpt_data.is_first_step = cache.first_time;
  sculpt_data.step = cache.step;
  //sculpt_data.local_transform = calc_local_space_matrix(/* TODO */);
  sculpt_data.texture_transform = calc_texture_space_matrix(cache);
  sculpt_data.depsgraph = &depsgraph;
  sculpt_data.self_object = &object;

  nodes::GeoNodesCallData call_data;
  call_data.root_ntree = &tree;
  call_data.side_effect_nodes = {};
  call_data.sculpt_data = &sculpt_data;

  bke::SculptingComputeContext compute_context;

  nodes::GeoNodesLFUserData user_data;
  user_data.call_data = &call_data;
  user_data.compute_context = &compute_context;

  LinearAllocator<> allocator;
  Vector<GMutablePointer> inputs_to_destruct;

  tree.ensure_interface_cache();

  /* Prepare main inputs. */
  for (const int i : tree.interface_inputs().index_range()) {
    const bNodeTreeInterfaceSocket &interface_socket = *tree.interface_inputs()[i];
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
  Array<bool> output_used_inputs(tree.interface_outputs().size(), true);
  for (const int i : tree.interface_outputs().index_range()) {
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

  fn::Field<T> output_field = output.get<fn::Field<T>>();
  fn::FieldEvaluator evaluator{context, outputs.size()};
  evaluator.add_with_destination(output_field, outputs);
  evaluator.evaluate();

  for (const int i : param_outputs.index_range()) {
    if (param_set_outputs[i]) {
      GMutablePointer &ptr = param_outputs[i];
      ptr.destruct();
    }
  }
}

template <typename T>
void mesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                Object &object,
                                StrokeCache &cache,
                                const Span<float3> position_eval,
                                const Span<int> verts,
                                MutableSpan<T> translations)
{
  Array<float3> positions(verts.size());

  for (const int i : positions.index_range()) {
    positions[i] = position_eval[verts[i]];
  }

  const Mesh *mesh = static_cast<const Mesh *>(object.data);
  bke::MeshSculptFieldContext context(depsgraph, object, *mesh, positions, verts);

  sculpt_nodes_evaluate<T>(depsgraph, object, cache, context, translations);
}

template void mesh_sculpt_nodes_evaluate<float3>(const Depsgraph& depsgraph,
                                Object& object,
                                StrokeCache& cache,
                                const Span<float3> position_eval,
                                const Span<int> verts,
                                MutableSpan<float3> translations);

template void mesh_sculpt_nodes_evaluate<float3>(const Depsgraph& depsgraph,
                                Object& object,
                                StrokeCache& cache,
                                const Span<float3> position_eval,
                                const Span<int> verts,
                                MutableSpan<float3> factors);

template <typename T>
void grids_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 StrokeCache &cache,
                                 SubdivCCG &subdiv_ccg,
                                 Span<int> grids,
                                 Span<float3> positions,
                                 MutableSpan<T> translations)
{
  bke::GridsSculptFieldContext context(depsgraph, object, subdiv_ccg, grids, positions);
  sculpt_nodes_evaluate<T>(depsgraph, object, cache, context, translations);
}


template void grids_sculpt_nodes_evaluate<float3>(const Depsgraph& depsgraph,
                                Object& object,
                                StrokeCache& cache,
                                SubdivCCG& subdiv_ccg,
                                Span<int> grids,
                                Span<float3> positions,
                                MutableSpan<float3> translations);

template void grids_sculpt_nodes_evaluate<float>(const Depsgraph& depsgraph,
                                Object& object,
                                StrokeCache& cache,
                                SubdivCCG& subdiv_ccg,
                                Span<int> grids,
                                Span<float3> positions,
                                MutableSpan<float> factors);

template <typename T>
void bmesh_sculpt_nodes_evaluate(const Depsgraph &depsgraph,
                                 Object &object,
                                 StrokeCache &cache,
                                 const Set<BMVert *, 0> &verts,
                                 Span<float3> positions,
                                 MutableSpan<T> translations)
{
  bke::BMeshSculptFieldContext context(depsgraph, object, verts, positions);
  sculpt_nodes_evaluate<T>(depsgraph, object, cache, context, translations);
}

template void bmesh_sculpt_nodes_evaluate<float3>(const Depsgraph& depsgraph,
  Object& object,
  StrokeCache& cache,
  const Set<BMVert*, 0>& verts,
  Span<float3> positions,
  MutableSpan<float3> translations);

template void bmesh_sculpt_nodes_evaluate<float>(const Depsgraph& depsgraph,
  Object& object,
  StrokeCache& cache,
  const Set<BMVert*, 0>& verts,
  Span<float3> positions,
  MutableSpan<float> factors);



}  // namespace blender::ed::sculpt_paint
