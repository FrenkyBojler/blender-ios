/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <memory>

#include "BLI_array_utils.hh"
#include "BLI_assert.h"
#include "BLI_generic_array.hh"
#include "BLI_generic_pointer.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_screen_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "NOD_geometry_nodes_dependencies.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"
#include "BKE_type_conversions.hh"

#include "FN_field.hh"
#include "FN_lazy_function_execute.hh"
#include "FN_multi_function_builder.hh"

#include "editors/sculpt_paint/mesh_brush_common.hh"
#include "sculpt_intern.hh"
#include "sculpt_nodes_evaluation.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint {

static bool is_socket_type_supported(const eNodeSocketDatatype type)
{
  return ELEM(type, SOCK_VECTOR, SOCK_RGBA, SOCK_FLOAT);
}

enum class OutputType : int8_t {
  MixFactors,
  Translations,
};

class FactorsFieldInput final : public fn::FieldInput {
 public:
  FactorsFieldInput() : FieldInput(CPPType::get<float>(), "Input Factors") {}

  GVArray get_varray_for_context(const fn::FieldContext & /*context*/,
                                 const IndexMask & /*mask*/,
                                 ResourceScope & /*scope*/) const override
  {
    /* Factors input should be handled by the field context since the factors vary per BVH node. */
    BLI_assert_unreachable();
    return {};
  }
};

static Depsgraph *build_extra_depsgraph(const Depsgraph &depsgraph_active, const Set<ID *> &ids)
{
  Depsgraph *depsgraph = DEG_graph_new(DEG_get_bmain(&depsgraph_active),
                                       DEG_get_input_scene(&depsgraph_active),
                                       DEG_get_input_view_layer(&depsgraph_active),
                                       DEG_get_mode(&depsgraph_active));
  DEG_graph_build_from_ids(depsgraph, Vector<ID *>(ids.begin(), ids.end()));
  DEG_evaluate_on_refresh(depsgraph);
  return depsgraph;
}

/**
 * Evaluates the Geometry Nodes node group associated with the specified brush in the given
 * context.
 *
 * Outputs a field that can be evaluated later for a specific node.
 */
static std::unique_ptr<NodeFieldEvalData> prepare_field_eval_data(const Scene &scene,
                                                                  const ARegion &region,
                                                                  const Depsgraph &depsgraph,
                                                                  const Object &object,
                                                                  const Brush &brush,
                                                                  const StrokeCache &cache,
                                                                  const OutputType output_type)
{
  const bNodeTree *orig_tree = brush.node_group;
  if (orig_tree == nullptr) {
    return {};
  }

  const bNodeTree *tree = nullptr;
  Depsgraph *depsgraph_extra = nullptr;

  Set<ID *> extra_ids;

  /* Gather dependencies from the node tree. */
  for (ID *id : orig_tree->runtime->geometry_nodes_eval_dependencies->ids.values()) {
    extra_ids.add(id);
  }

  if (!extra_ids.is_empty()) {
    extra_ids.add(const_cast<ID *>(&orig_tree->id));

    /* Build a separate depsgraph for the dependencies */
    depsgraph_extra = build_extra_depsgraph(depsgraph, extra_ids);

    /* Get the depsgraph-evaluated version of the node tree */
    tree = reinterpret_cast<const bNodeTree *>(
        DEG_get_evaluated_id(depsgraph_extra, const_cast<ID *>(&orig_tree->id)));
  }
  else {
    tree = orig_tree;
  }

  const nodes::GeometryNodesLazyFunctionGraphInfo &lf_graph_info =
      *nodes::ensure_geometry_nodes_lazy_function_graph(*tree);
  const nodes::GeometryNodesGroupFunction &function = lf_graph_info.function;
  const lf::LazyFunction &lazy_function = *function.function;
  const int num_inputs = lazy_function.inputs().size();
  const int num_outputs = lazy_function.outputs().size();

  /* Nothing to do. */
  if (num_outputs == 0) {
    return {};
  }

  const bNodeTreeInterfaceSocket *first_output = tree->interface_outputs()[0];
  const eNodeSocketDatatype type = (eNodeSocketDatatype)first_output->socket_typeinfo()->type;

  /* Output type is unsupported. TODO: warn user? */
  if (!is_socket_type_supported(type)) {
    return {};
  }

  auto output = std::make_unique<NodeFieldEvalData>();

  Array<GMutablePointer> param_inputs(num_inputs);
  Array<std::optional<lf::ValueUsage>> param_input_usages(num_inputs);
  Array<lf::ValueUsage> param_output_usages(num_outputs);

  MutableSpan<GMutablePointer> param_outputs = output->scope.construct<Array<GMutablePointer>>(
      num_outputs);
  MutableSpan<bool> param_set_outputs = output->scope.construct<Array<bool>>(num_outputs, false);

  /* We want to evaluate the main outputs, but don't care about which inputs are used for now. */
  param_output_usages.as_mutable_span().slice(function.outputs.main).fill(lf::ValueUsage::Used);
  param_output_usages.as_mutable_span()
      .slice(function.outputs.input_usages)
      .fill(lf::ValueUsage::Unused);

  nodes::GeoNodesSculptData sculpt_data;
  sculpt_data.depsgraph = &depsgraph;
  sculpt_data.depsgraph_extra = depsgraph_extra;
  sculpt_data.self_object = &object;

  const RegionView3D &rv3d = reinterpret_cast<const RegionView3D &>(region.regiondata);

  sculpt_data.view_matrix = float4x4(rv3d.viewmat);
  sculpt_data.projection_matrix = float4x4(rv3d.winmat);
  sculpt_data.is_orthographic = !bool(rv3d.is_persp);

  sculpt_data.mouse_position = int2(int(cache.mouse_event.x), int(cache.mouse_event.y));

  sculpt_data.region_size = int2(region.winx, region.winy);

  sculpt_data.view_3d_cursor_location = scene.cursor.location;
  sculpt_data.view_3d_cursor_rotation = scene.cursor.rotation();

  nodes::GeoNodesCallData call_data;
  call_data.root_ntree = tree;
  call_data.side_effect_nodes = {};
  call_data.sculpt_data = &sculpt_data;

  bke::SculptComputeContext compute_context;

  nodes::GeoNodesUserData user_data;
  user_data.call_data = &call_data;
  user_data.compute_context = &compute_context;

  LinearAllocator<> &allocator = output->scope.allocator();
  Vector<GMutablePointer> inputs_to_destruct;

  tree->ensure_interface_cache();

  /* Prepare main inputs. */
  for (const int i : tree->interface_inputs().index_range()) {
    const bNodeTreeInterfaceSocket &interface_socket = *tree->interface_inputs()[i];
    const bke::bNodeSocketType *typeinfo = interface_socket.socket_typeinfo();

    const CPPType *type = typeinfo->geometry_nodes_cpp_type;
    BLI_assert(type != nullptr);
    void *value = allocator.allocate(*type);

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
    param_outputs[i] = {type, allocator.allocate(type)};
  }

  nodes::GeoNodesLocalUserData local_user_data(user_data);

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
  switch (output_type) {
    case OutputType::MixFactors: {
      fn::Field<float> converted = conversions.try_convert(std::move(output_field),
                                                           CPPType::get<float>());
      static auto multiply_fn = mf::build::SI2_SO<float, float, float>(
          "Multiply",
          [](float a, float b) { return a * b; },
          mf::build::exec_presets::AllSpanOrSingle());

      fn::Field<float> input_factors(std::make_shared<FactorsFieldInput>());
      fn::Field<float> final_factor{
          fn::FieldOperation::from(multiply_fn, {std::move(converted), std::move(input_factors)})};
      output->field = std::move(final_factor);
      break;
    }
    case OutputType::Translations: {
      fn::Field<float3> converted = conversions.try_convert(std::move(output_field),
                                                            CPPType::get<float3>());
      output->field = std::move(converted);
      break;
    }
  }

  /* Destruct inputs and outputs */
  for (GMutablePointer &ptr : inputs_to_destruct) {
    ptr.destruct();
  }

  output->scope.add_destruct_call([param_set_outputs, param_outputs]() {
    for (const int i : param_outputs.index_range()) {
      if (param_set_outputs[i]) {
        GMutablePointer &ptr = param_outputs[i];
        ptr.destruct();
      }
    }
  });

  return output;
}

std::unique_ptr<NodeFieldEvalData> prepare_field_eval_data(const Scene &scene,
                                                           const ARegion &region,
                                                           const Depsgraph &depsgraph,
                                                           const Object &object,
                                                           const Brush &brush,
                                                           const StrokeCache &cache)
{
  if (brush_uses_vector_displacement(brush)) {
    return prepare_field_eval_data(
        scene, region, depsgraph, object, brush, cache, OutputType::Translations);
  }
  return prepare_field_eval_data(
      scene, region, depsgraph, object, brush, cache, OutputType::MixFactors);
}

class MeshSculptFieldContext : public fn::FieldContext {
  const Mesh &mesh_;
  Span<int> verts_;
  Span<float3> vert_positions_;
  Span<float3> vert_normals_;
  Span<float> factors_;

 public:
  MeshSculptFieldContext(const Depsgraph &depsgraph,
                         const Object &object,
                         const Span<int> verts,
                         const Span<float3> vert_positions,
                         const Span<float> factors)
      : fn::FieldContext(),
        mesh_(*static_cast<const Mesh *>(object.data)),
        verts_(verts),
        vert_positions_(vert_positions),
        vert_normals_(bke::pbvh::vert_normals_eval(depsgraph, object)),
        factors_(factors)
  {
  }

  VArray<float3> positions() const
  {
    Array<float3> positions(verts_.size());
    gather_data_mesh(vert_positions_, verts_, positions.as_mutable_span());
    return VArray<float3>::from_container(std::move(positions));
  }

  VArray<float3> normals() const
  {
    Array<float3> normals(verts_.size());
    gather_data_mesh(vert_normals_, verts_, normals.as_mutable_span());
    return VArray<float3>::from_container(std::move(normals));
  }

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope &scope) const override
  {
    if (const auto *attr = dynamic_cast<const bke::AttributeFieldInput *>(&field_input)) {
      if (attr->attribute_name() == "position") {
        return this->positions();
      }
      if (const bke::GAttributeReader attribute = mesh_.attributes().lookup(
              attr->attribute_name(), bke::AttrDomain::Point))
      {
        GArray<> compressed(attribute.varray.type(), verts_.size());
        bke::attribute_math::gather(attribute.varray, verts_, compressed.as_mutable_span());
        return GVArray::from_garray(std::move(compressed));
      }
      return {};
    }
    if (dynamic_cast<const bke::NormalFieldInput *>(&field_input)) {
      return this->normals();
    }
    if (dynamic_cast<const fn::IndexFieldInput *>(&field_input)) {
      return VArray<int>::from_span(verts_);
    }
    if (dynamic_cast<const bke::IDAttributeFieldInput *>(&field_input)) {
      if (const VArray<int> id = *mesh_.attributes().lookup<int>("id", bke::AttrDomain::Point)) {
        Array<int> compressed(verts_.size());
        array_utils::gather(id, verts_, compressed.as_mutable_span());
        return VArray<int>::from_container(std::move(compressed));
      }
      return VArray<int>::from_span(verts_);
    }
    if (dynamic_cast<const FactorsFieldInput *>(&field_input)) {
      return VArray<float>::from_span(factors_);
    }
    return field_input.get_varray_for_context(*this, mask, scope);
  }
};

void nodes_evaluate_translations_mesh(const Depsgraph &depsgraph,
                                      const Object &object,
                                      const StrokeCache &cache,
                                      const Span<float3> vert_positions,
                                      const Span<int> verts,
                                      const MutableSpan<float3> translations)
{
  if (!cache.node_field_eval_data) {
    return;
  }
  threading::isolate_task([&]() {
    const MeshSculptFieldContext context(depsgraph, object, verts, vert_positions, {});
    fn::FieldEvaluator evaluator{context, verts.size()};
    evaluator.add_with_destination(cache.node_field_eval_data->field, translations);
    evaluator.evaluate();
  });
}

void nodes_evaluate_factors_mesh(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const StrokeCache &cache,
                                 const Span<float3> vert_positions,
                                 const Span<int> verts,
                                 const MutableSpan<float> factors)
{
  if (!cache.node_field_eval_data) {
    return;
  }
  threading::isolate_task([&]() {
    const MeshSculptFieldContext context(depsgraph, object, verts, vert_positions, factors);
    fn::FieldEvaluator evaluator{context, verts.size()};
    evaluator.add_with_destination(cache.node_field_eval_data->field, factors);
    evaluator.evaluate();
  });
}

class GridsSculptFieldContext : public fn::FieldContext {
  const SubdivCCG &subdiv_ccg_;
  Span<float3> positions_;
  Span<int> grids_;
  Span<float> factors_;

 public:
  GridsSculptFieldContext(const SubdivCCG &subdiv_ccg,
                          const Span<int> grids,
                          const Span<float3> positions,
                          const Span<float> factors)
      : fn::FieldContext(),
        subdiv_ccg_(subdiv_ccg),
        positions_(positions),
        grids_(grids),
        factors_(factors)
  {
  }

  VArray<float3> normals() const
  {
    const int verts_num = grids_.size() * subdiv_ccg_.grid_area;
    Array<float3> normals(verts_num);
    gather_grids_normals(subdiv_ccg_, grids_, normals);
    return VArray<float3>::from_container(std::move(normals));
  }

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope &scope) const override
  {
    if (const auto *attr = dynamic_cast<const bke::AttributeFieldInput *>(&field_input)) {
      if (attr->attribute_name() == "position") {
        return VArray<float3>::from_span(positions_);
      }
    }
    if (dynamic_cast<const bke::NormalFieldInput *>(&field_input)) {
      return this->normals();
    }
    if (dynamic_cast<const FactorsFieldInput *>(&field_input)) {
      return VArray<float>::from_span(factors_);
    }
    return field_input.get_varray_for_context(*this, mask, scope);
  }
};

void nodes_evaluate_factors_grids(const StrokeCache &cache,
                                  const SubdivCCG &subdiv_ccg,
                                  const Span<int> grids,
                                  const Span<float3> positions,
                                  const MutableSpan<float> factors)
{
  if (!cache.node_field_eval_data) {
    return;
  }
  threading::isolate_task([&]() {
    const GridsSculptFieldContext context(subdiv_ccg, grids, positions, factors);
    fn::FieldEvaluator evaluator{context, positions.size()};
    evaluator.add_with_destination(cache.node_field_eval_data->field, factors);
    evaluator.evaluate();
  });
}

void nodes_evaluate_translations_grids(const StrokeCache &cache,
                                       const SubdivCCG &subdiv_ccg,
                                       const Span<int> grids,
                                       const Span<float3> positions,
                                       const MutableSpan<float3> translations)
{
  if (!cache.node_field_eval_data) {
    return;
  }
  threading::isolate_task([&]() {
    const GridsSculptFieldContext context(subdiv_ccg, grids, positions, {});
    fn::FieldEvaluator evaluator{context, positions.size()};
    evaluator.add_with_destination(cache.node_field_eval_data->field, translations);
    evaluator.evaluate();
  });
}

class BMeshSculptFieldContext : public fn::FieldContext {
  const Set<BMVert *, 0> &verts_;
  Span<float3> positions_;
  Span<float> factors_;

 public:
  BMeshSculptFieldContext(const Set<BMVert *, 0> &verts,
                          const Span<float3> positions,
                          const Span<float> factors)
      : fn::FieldContext(), verts_(verts), positions_(positions), factors_(factors)
  {
  }

  VArray<float3> normals() const
  {
    Array<float3> normals(verts_.size());
    gather_bmesh_normals(verts_, normals);
    return VArray<float3>::from_container(std::move(normals));
  }

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope &scope) const override
  {
    if (const auto *attr = dynamic_cast<const bke::AttributeFieldInput *>(&field_input)) {
      if (attr->attribute_name() == "position") {
        return VArray<float3>::from_span(positions_);
      }
    }
    if (dynamic_cast<const bke::NormalFieldInput *>(&field_input)) {
      return this->normals();
    }
    if (dynamic_cast<const FactorsFieldInput *>(&field_input)) {
      return VArray<float>::from_span(factors_);
    }
    return field_input.get_varray_for_context(*this, mask, scope);
  }
};

void nodes_evaluate_factors_bmesh(const StrokeCache &cache,
                                  const Set<BMVert *, 0> &verts,
                                  const Span<float3> positions,
                                  const MutableSpan<float> factors)
{
  if (!cache.node_field_eval_data) {
    return;
  }
  threading::isolate_task([&]() {
    const BMeshSculptFieldContext context(verts, positions, factors);
    fn::FieldEvaluator evaluator{context, positions.size()};
    evaluator.add_with_destination(cache.node_field_eval_data->field, factors);
    evaluator.evaluate();
  });
}

void nodes_evaluate_translations_bmesh(const StrokeCache &cache,
                                       const Set<BMVert *, 0> &verts,
                                       const Span<float3> positions,
                                       const MutableSpan<float3> translations)
{
  if (!cache.node_field_eval_data) {
    return;
  }
  threading::isolate_task([&]() {
    const BMeshSculptFieldContext context(verts, positions, {});
    fn::FieldEvaluator evaluator{context, positions.size()};
    evaluator.add_with_destination(cache.node_field_eval_data->field, translations);
    evaluator.evaluate();
  });
}

}  // namespace blender::ed::sculpt_paint
