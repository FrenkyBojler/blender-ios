/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "BLI_array_utils.hh"

#include "BKE_anonymous_attribute_make.hh"
#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_editmesh.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"
#include "BKE_type_conversions.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_mesh_types.h"

#include "FN_field.hh"

#include "GEO_hair_simulation.hh"
#include "GEO_hair_solver.hh"
#include "GEO_reverse_uv_sampler.hh"

#include "NOD_geo_hair_constraints.hh"
#include "NOD_geometry_nodes_closure_eval.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_hair_simulation_cc {

using geometry::hair_constraints::ConstraintEvalParams;
using geometry::hair_constraints::ConstraintType;
using geometry::hair_constraints::ConstraintTypeInfo;
using geometry::hair_constraints::ConstraintVariables;
using geometry::hair_solver::ConstraintEvalData;
using geometry::hair_solver::VariableIndexArrays;
using hair_constraints::ConstraintBundleItems;
namespace hairsim = geometry::hair_simulation;

/* Curve geometry attributes. */
static const std::string rest_position_attr = "rest_position";
static const std::string rest_rotation_attr = "rest_rotation";
static const std::string radius_attr = "radius";
/* Constraint point attributes. */
static const std::string target_point1_attr = "point1";
static const std::string target_point2_attr = "point2";
static const std::string target_point3_attr = "point3";
static const std::string target_point4_attr = "point4";
enum TargetPointAttribute {
  TargetPoint1,
  TargetPoint2,
  TargetPoint3,
  TargetPoint4,
};
static StringRef target_point_attribute(const TargetPointAttribute target_point_attr)
{
  switch (target_point_attr) {
    case TargetPoint1:
      return target_point1_attr;
    case TargetPoint2:
      return target_point2_attr;
    case TargetPoint3:
      return target_point3_attr;
    case TargetPoint4:
      return target_point4_attr;
  }
  BLI_assert_unreachable();
  return "";
}

static const std::string surface_position_attr = "surface_position";
static const std::string surface_rotation_attr = "surface_rotation";
static const std::string goal_position_attr = "goal_position";
static const std::string goal_rotation_attr = "goal_rotation";

/* XXX These should be anonymous attributes. */
static const std::string old_position_attr = ".old_position";
static const std::string old_rotation_attr = ".old_rotation";
static const std::string cross_section_attr = ".cross_section";
static const std::string area_moment_attr = ".area_moment";
/* Segment length attribute for material calculation purposes. */
static const std::string material_length_attr = ".material_length";

namespace field_constants {

template<typename T> static Field<T> constant_field(const T &value)
{
  return Field<T>{std::make_shared<fn::FieldConstant>(CPPType::get<T>(), &value)};
}

static Field<float3> zero_vector()
{
  return constant_field<float3>(float3(0.0f));
}

}  // namespace field_constants

namespace field_inputs {

static Field<float3> rest_position()
{
  return bke::AttributeFieldInput::Create<float3>(rest_position_attr);
}

static Field<math::Quaternion> rest_rotation()
{
  return bke::AttributeFieldInput::Create<math::Quaternion>(rest_rotation_attr);
}

static Field<float3> old_position()
{
  return bke::AttributeFieldInput::Create<float3>(old_position_attr);
}

static Field<math::Quaternion> old_rotation()
{
  return bke::AttributeFieldInput::Create<math::Quaternion>(old_rotation_attr);
}

static Field<float> radius()
{
  return bke::AttributeFieldInput::Create<float>(radius_attr);
}

static Field<float> material_length()
{
  return bke::AttributeFieldInput::Create<float>(material_length_attr);
}

static Field<int> UNUSED_FUNCTION(target_point)(const TargetPointAttribute target_point_attr)
{
  return bke::AttributeFieldInput::Create<int>(target_point_attribute(target_point_attr));
}

}  // namespace field_inputs

enum class VectorSpace {
  /* Object space. */
  Object,
  /* Local space of an elemental rigid body, aligned with principal axes. */
  BodyLocal,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("Delta Time").min(0.0f).hide_value();
  b.add_input<decl::Int>("Constraint Iterations").default_value(5).min(0);

  b.add_input<decl::Geometry>("Hair").supported_type(bke::GeometryComponent::Type::Curve);
  b.add_output<decl::Geometry>("Hair").propagate_all().align_with_previous();

  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();

  b.add_input<decl::Bundle>("Data").description("Simulation state from the previous iteration");
  b.add_output<decl::Bundle>("Data")
      .description("Simulation state that should be passed to the next iteration")
      .align_with_previous();

  b.add_input<decl::Bundle>("Behavior")
      .description("Parameters for controlling simulation behavior");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  UNUSED_VARS(layout, ptr);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  UNUSED_VARS(node);
}

template<typename T> struct VariantConverter {
  static std::optional<T> convert(const SocketValueVariant &variant)
  {
    if (!variant.is_single()) {
      return std::nullopt;
    }

    std::optional<eNodeSocketDatatype> socket_type = bke::geo_nodes_base_cpp_type_to_socket_type(
        CPPType::get<T>());
    if (!socket_type) {
      return std::nullopt;
    }
    if (!variant.valid_for_socket(*socket_type)) {
      return std::nullopt;
    }
    return variant.get<T>();
  }
};

template<typename U> struct VariantConverter<Field<U>> {
  static std::optional<Field<U>> convert(const SocketValueVariant &variant)
  {
    if (!variant.is_context_dependent_field()) {
      std::optional<U> single_value = VariantConverter<U>::convert(variant);
      if (!single_value) {
        return std::nullopt;
      }
      return Field<U>{std::make_shared<fn::FieldConstant>(CPPType::get<U>(), &(*single_value))};
    }

    std::optional<eNodeSocketDatatype> socket_type = bke::geo_nodes_base_cpp_type_to_socket_type(
        CPPType::get<U>());
    if (!variant.valid_for_socket(*socket_type)) {
      return std::nullopt;
    }
    return variant.get<Field<U>>();
  }
};

template<typename T>
static std::optional<T> get_from_bundle(const BundlePtr &bundle, const StringRef name)
{
  if (!bundle) {
    return std::nullopt;
  }

  BLI_assert(!name.is_empty());
  const std::optional<Bundle::Item> value = bundle->lookup(SocketInterfaceKey(name));
  if (!value) {
    return std::nullopt;
  }

  if constexpr (GeoNodeExecParams::stored_as_SocketValueVariant_v<T>) {
    // TODO This does not support implicit conversions yet!
    // The type of the variant (bundle item) has to match the parameter T exactly.
    if (value->type->geometry_nodes_cpp_type == &CPPType::get<SocketValueVariant>()) {
      const SocketValueVariant &variant = *static_cast<const SocketValueVariant *>(value->value);
      return VariantConverter<T>::convert(variant);
    }
    return std::nullopt;
  }
  else {
    if (value->type->geometry_nodes_cpp_type == &CPPType::get<GeometrySet>()) {
      if constexpr (std::is_same_v<T, GeometrySet>) {
        return *static_cast<const GeometrySet *>(value->value);
      }
    }
    else if (value->type->geometry_nodes_cpp_type == &CPPType::get<T>()) {
      return *static_cast<const T *>(value->value);
    }
  }
  return std::nullopt;
}

template<typename T>
static bool get_from_bundle(const BundlePtr &bundle, const StringRef name, T &result)
{
  if (std::optional<T> value = get_from_bundle<T>(bundle, name)) {
    result = *value;
    return true;
  }
  return false;
}

struct LazyFunctionIndices {
  struct {
    Vector<int> main_indices;
    Vector<int> output_usage_indices;
  } inputs;
  struct {
    Vector<int> main_indices;
    Vector<int> input_usage_indices;
  } outputs;
};

struct ClosureInputItem {
  StringRef name;
  eNodeSocketDatatype type;
};
struct ClosureOutputItem {
  StringRef name;
  eNodeSocketDatatype type;
};

class LazyFunctionForClosure : public LazyFunction {
 protected:
  int32_t node_identifier_;
  LazyFunctionIndices lf_indices_;
  Vector<ClosureInputItem> input_items_;
  Vector<ClosureOutputItem> output_items_;

 public:
  LazyFunctionForClosure(const int32_t node_identifier, const char *debug_name)
      : node_identifier_(node_identifier)
  {
    debug_name_ = debug_name;
  }

  const LazyFunctionIndices &lf_indices() const
  {
    return lf_indices_;
  }

  Span<ClosureInputItem> input_items() const
  {
    return input_items_;
  }

  Span<ClosureOutputItem> output_items() const
  {
    return output_items_;
  }

  int add_input(const char *name, const eNodeSocketDatatype socket_type)
  {
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(socket_type);
    BLI_assert(stype != nullptr);

    const int main_index = inputs_.append_and_get_index_as(
        name, *stype->geometry_nodes_cpp_type, lf::ValueUsage::Maybe);
    const int used_index = outputs_.append_and_get_index_as("Usage", CPPType::get<bool>());

    lf_indices_.inputs.main_indices.append(main_index);
    lf_indices_.outputs.input_usage_indices.append(used_index);

    input_items_.append(ClosureInputItem{name, socket_type});

    return main_index;
  }

  int add_output(const char *name, const eNodeSocketDatatype socket_type)
  {
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(socket_type);
    BLI_assert(stype != nullptr);

    const int main_index = outputs_.append_and_get_index_as(name, *stype->geometry_nodes_cpp_type);
    const int used_index = inputs_.append_and_get_index_as(
        "Usage", CPPType::get<bool>(), lf::ValueUsage::Maybe);
    lf_indices_.outputs.main_indices.append(main_index);
    lf_indices_.inputs.output_usage_indices.append(used_index);

    output_items_.append(ClosureOutputItem{name, socket_type});

    return main_index;
  }
};

static ClosurePtr create_closure_for_lazy_function(const LazyFunctionForClosure &body_fn)
{
  const LazyFunctionIndices &lf_indices = body_fn.lf_indices();
  const Span<ClosureInputItem> inputs = body_fn.input_items();
  const Span<ClosureOutputItem> outputs = body_fn.output_items();

  std::shared_ptr<ClosureSignature> closure_signature = std::make_shared<ClosureSignature>();
  for (const ClosureInputItem &item : inputs) {
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.type);
    closure_signature->inputs.append({SocketInterfaceKey(item.name), stype});
  }
  for (const ClosureOutputItem &item : outputs) {
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.type);
    closure_signature->outputs.append({SocketInterfaceKey(item.name), stype});
  }

  std::unique_ptr<ResourceScope> closure_scope = std::make_unique<ResourceScope>();

  lf::Graph &lf_graph = closure_scope->construct<lf::Graph>("Closure Graph");
  lf::FunctionNode &lf_body_node = lf_graph.add_function(body_fn);
  ClosureFunctionIndices closure_indices;
  Vector<const void *> default_input_values;

  for (const int i : inputs.index_range()) {
    const ClosureInputItem &item = inputs[i];
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.type);
    const CPPType &cpp_type = *stype->geometry_nodes_cpp_type;

    lf::GraphInputSocket &lf_graph_input = lf_graph.add_input(cpp_type, item.name);
    lf_graph.add_link(lf_graph_input, lf_body_node.input(lf_indices.inputs.main_indices[i]));

    lf::GraphOutputSocket &lf_graph_input_usage = lf_graph.add_output(
        CPPType::get<bool>(), "Usage: " + StringRef(item.name));
    lf_graph.add_link(lf_body_node.output(lf_indices.outputs.input_usage_indices[i]),
                      lf_graph_input_usage);

    void *default_value = closure_scope->allocate_owned(cpp_type);
    construct_socket_default_value(*stype, default_value);
    default_input_values.append(default_value);
  }
  closure_indices.inputs.main = lf_graph.graph_inputs().index_range().take_back(inputs.size());
  closure_indices.outputs.input_usages = lf_graph.graph_outputs().index_range().take_back(
      inputs.size());

  for (const int i : outputs.index_range()) {
    const ClosureOutputItem &item = outputs[i];
    const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.type);
    const CPPType &cpp_type = *stype->geometry_nodes_cpp_type;

    lf::GraphOutputSocket &lf_graph_output = lf_graph.add_output(cpp_type, item.name);
    lf_graph.add_link(lf_body_node.output(lf_indices.outputs.main_indices[i]), lf_graph_output);

    lf::GraphInputSocket &lf_graph_output_usage = lf_graph.add_input(
        CPPType::get<bool>(), "Usage: " + StringRef(item.name));
    lf_graph.add_link(lf_graph_output_usage,
                      lf_body_node.input(lf_indices.inputs.output_usage_indices[i]));
  }
  closure_indices.outputs.main = lf_graph.graph_outputs().index_range().take_back(outputs.size());
  closure_indices.inputs.output_usages = lf_graph.graph_inputs().index_range().take_back(
      outputs.size());

  lf_graph.update_node_indices();

  lf::GraphExecutor &lf_graph_executor = closure_scope->construct<lf::GraphExecutor>(
      lf_graph, nullptr, nullptr, nullptr);
  ClosurePtr closure{MEM_new<Closure>(__func__,
                                      closure_signature,
                                      std::move(closure_scope),
                                      lf_graph_executor,
                                      closure_indices,
                                      std::move(default_input_values),
                                      std::nullopt /*source_location*/,
                                      std::make_shared<ClosureEvalLog>())};

  return closure;
}

class LazyFunctionForCurveConstraintUpdate : public LazyFunctionForClosure {
 private:
  int input_geometry_index_;
  int input_stretch_index_, input_bend_index_;
  int output_stretch_index_, output_bend_index_;

 public:
  LazyFunctionForCurveConstraintUpdate(const int32_t node_identifier, const char *debug_name)
      : LazyFunctionForClosure(node_identifier, debug_name)
  {
    input_geometry_index_ = add_input("Geometry", SOCK_GEOMETRY);
    input_stretch_index_ = add_input("Stretch Constraints", SOCK_GEOMETRY);
    input_bend_index_ = add_input("Bend Constraints", SOCK_GEOMETRY);
    output_stretch_index_ = add_output("Stretch Constraints", SOCK_GEOMETRY);
    output_bend_index_ = add_output("Bend Constraints", SOCK_GEOMETRY);
  }

  void execute_impl(lf::Params &params, const lf::Context & /*context*/) const override
  {
    // const ScopedNodeTimer node_timer{context, node_};

    // GeoNodesUserData *user_data = dynamic_cast<GeoNodesUserData *>(context.user_data);
    // BLI_assert(user_data != nullptr);

    GeometrySet stretch_constraints = params.get_input<GeometrySet>(input_stretch_index_);
    GeometrySet bend_constraints = params.get_input<GeometrySet>(input_bend_index_);
    params.set_output(output_stretch_index_, std::move(stretch_constraints));
    params.set_output(output_bend_index_, std::move(bend_constraints));
  }
};

struct Error {
  NodeWarningType type;
  std::string message;
};

template<typename T> struct ResultOrError {
  T value;
  std::optional<Error> error;

  ResultOrError(const T &value) : value(value) {}
  ResultOrError(T &&value) : value(std::move(value)) {}
  ResultOrError(const NodeWarningType type, StringRefNull message)
      : error(std::make_optional<Error>({type, message}))
  {
  }
  ResultOrError(const Error &error) : error(error) {}
  ResultOrError(Error &&error) : error(std::move(error)) {}
  ResultOrError(const std::optional<Error> &error) : error(error) {}
  ResultOrError(std::optional<Error> &&error) : error(std::move(error)) {}
};

// TODO share this with node_geo_deform_curves_on_surface.cc
// TODO make it non-copyable? (prevent potential double free of allocated mesh data)
class HairSurfaceData {
 public:
  Mesh *surface_mesh_ = nullptr;
  bool free_suface_mesh_ = false;

  StringRefNull uv_map_name_;
  StringRefNull rest_position_name_;

  VArraySpan<float2> uv_map_;
  VArraySpan<float3> rest_positions_;

  bke::CurvesSurfaceTransforms transforms_;

 public:
  HairSurfaceData() = default;
  HairSurfaceData(HairSurfaceData &&other) = default;
  ~HairSurfaceData()
  {
    if (free_suface_mesh_) {
      BKE_id_free(nullptr, surface_mesh_);
    }
  }

  bool is_valid() const
  {
    return surface_mesh_ != nullptr;
  }

  operator bool() const
  {
    return is_valid();
  }

  Mesh &surface_mesh() const
  {
    return *surface_mesh_;
  }

  const bke::CurvesSurfaceTransforms &transforms() const
  {
    return transforms_;
  }

  Span<float2> uv_map() const
  {
    return uv_map_;
  }
  Span<float3> rest_positions() const
  {
    return rest_positions_;
  }

  geometry::ReverseUVSampler reverse_uv_sampler() const
  {
    return geometry::ReverseUVSampler{uv_map_, surface_mesh_->corner_tris()};
  }

  std::optional<Error> init_from_object(const Object *self_ob_eval, const bool use_orig_mesh)
  {
    if (self_ob_eval == nullptr || self_ob_eval->type != OB_CURVES) {
      return Error{NodeWarningType::Error, TIP_("Node only works for curves objects")};
    }
    const Curves *self_curves_eval = static_cast<const Curves *>(self_ob_eval->data);
    if (self_curves_eval->surface_uv_map == nullptr || self_curves_eval->surface_uv_map[0] == '\0')
    {
      return Error{NodeWarningType::Error, TIP_("Surface UV map not defined")};
    }
    /* Take surface information from self-object. */
    Object *surface_ob_eval = self_curves_eval->surface;
    if (surface_ob_eval == nullptr || surface_ob_eval->type != OB_MESH) {
      return Error{NodeWarningType::Error, TIP_("Curves not attached to a surface")};
    }

    Mesh *surface_mesh;
    bool free_suface_mesh;
    if (use_orig_mesh) {
      Object *surface_ob_orig = DEG_get_original(surface_ob_eval);
      Mesh &surface_object_data = *static_cast<Mesh *>(surface_ob_orig->data);
      if (BMEditMesh *em = surface_object_data.runtime->edit_mesh.get()) {
        surface_mesh = BKE_mesh_from_bmesh_for_eval_nomain(em->bm, nullptr, &surface_object_data);
        free_suface_mesh = true;
      }
      else {
        surface_mesh = &surface_object_data;
        free_suface_mesh = false;
      }
    }
    else {
      surface_mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(surface_ob_eval);
      free_suface_mesh = false;
      if (surface_mesh == nullptr) {
        return Error{NodeWarningType::Error, TIP_("Surface has no mesh")};
      }
      BKE_mesh_wrapper_ensure_mdata(surface_mesh);
    }

    const AttributeAccessor mesh_attributes = surface_mesh->attributes();
    const StringRefNull uv_map_name = self_curves_eval->surface_uv_map;
    const StringRefNull rest_position_name = "rest_position";

    if (use_orig_mesh) {
      if (!mesh_attributes.contains(uv_map_name)) {
        return Error{NodeWarningType::Error,
                     fmt::format(fmt::runtime(TIP_("Original surface missing UV map: \"{}\"")),
                                 uv_map_name)};
      }
    }
    else {
      if (!mesh_attributes.contains(uv_map_name)) {
        return Error{NodeWarningType::Error,
                     fmt::format(fmt::runtime(TIP_("Evaluated surface missing UV map: \"{}\"")),
                                 uv_map_name)};
      }
      if (!mesh_attributes.contains(rest_position_name)) {
        return Error{NodeWarningType::Error,
                     fmt::format(fmt::runtime(TIP_("Evaluated surface missing attribute: \"{}\"")),
                                 rest_position_name)};
      }
    }

    surface_mesh_ = surface_mesh;
    free_suface_mesh_ = free_suface_mesh;
    transforms_ = bke::CurvesSurfaceTransforms{*self_ob_eval, surface_ob_eval};
    uv_map_name_ = uv_map_name;
    rest_position_name_ = rest_position_name;
    uv_map_ = *surface_mesh_->attributes().lookup<float2>(uv_map_name_, AttrDomain::Corner);
    if (!use_orig_mesh) {
      rest_positions_ = *surface_mesh_->attributes().lookup<float3>(rest_position_name_,
                                                                    AttrDomain::Point);
    }
    return std::nullopt;
  }
};

/**
 * Construct a surface reference frame for each curve based on hair surface attachment.
 */
static Array<float4x4> compute_curve_surface_transforms(const bke::CurvesGeometry &curves,
                                                        const IndexMask &selection,
                                                        const HairSurfaceData &surface_data)
{
  const int curves_num = curves.curves_num();
  const AttributeAccessor attributes = curves.attributes();
  const float4x4 &curves_to_surface = surface_data.transforms().curves_to_surface;
  const float4x4 &surface_to_curves = surface_data.transforms().surface_to_curves;

  /* Find samples from UV coordinates. */
  const geometry::ReverseUVSampler reverse_uv_sampler = surface_data.reverse_uv_sampler();
  Array<geometry::ReverseUVSampler::Result> surface_samples(curves_num);
  const VArraySpan surface_uv_coords = *attributes.lookup_or_default<float2>(
      "surface_uv_coordinate", AttrDomain::Curve, float2(0));
  reverse_uv_sampler.sample_many(surface_uv_coords, surface_samples);

  const Span<float3> positions = surface_data.surface_mesh().vert_positions();
  /* The tangent reference direction is used to determine the rotation of
   * the surface point around its normal axis. It's important that the tangent directions are
   * computed in a consistent way. If the surface has not been rotated, the old and new tangent
   * reference have to have the same direction. For that reason, the tangent reference is
   * computed based on a reference position attribute instead of positions on the mesh. This way
   * the old and new tangent reference use the same topology.
   *
   * TODO: Figure out if this can be smoothly interpolated across the surface as well.
   * Currently, this is a source of discontinuity in the deformation, because the vector
   * changes instantly from one triangle to the next. #128184
   */
  const Span<float3> tangent_positions = surface_data.rest_positions().is_empty() ?
                                             surface_data.surface_mesh().vert_positions() :
                                             surface_data.rest_positions();
  const Span<int> corner_verts = surface_data.surface_mesh().corner_verts();
  /* Retrieve face corner normals from each mesh. It's necessary to use face corner normals
   * because face normals or vertex normals may lose information (custom normals, auto smooth) in
   * some cases. */
  const Span<float3> corner_normals = surface_data.surface_mesh().corner_normals();
  const Span<int3> corner_tris = surface_data.surface_mesh().corner_tris();

  /* Compute surface reference frame for each curve. */
  Array<float4x4> surface_transforms(curves_num);
  std::atomic<int> invalid_uv_count;
  selection.foreach_index(GrainSize(256), [&](const int curve_i) {
    const geometry::ReverseUVSampler::Result &surface_sample = surface_samples[curve_i];
    if (surface_sample.type != geometry::ReverseUVSampler::ResultType::Ok) {
      invalid_uv_count++;
      return;
    }

    const int3 &tri = corner_tris[surface_sample.tri_index];
    const float3 &bary_weights = surface_sample.bary_weights;

    const int corner_0 = tri[0];
    const int corner_1 = tri[1];
    const int corner_2 = tri[2];

    const int vert_0 = corner_verts[corner_0];
    const int vert_1 = corner_verts[corner_1];
    const int vert_2 = corner_verts[corner_2];

    const float3 &normal_0 = corner_normals[corner_0];
    const float3 &normal_1 = corner_normals[corner_1];
    const float3 &normal_2 = corner_normals[corner_2];
    const float3 normal = math::normalize(
        bke::attribute_math::mix3(bary_weights, normal_0, normal_1, normal_2));

    const float3 &pos_0 = positions[vert_0];
    const float3 &pos_1 = positions[vert_1];
    const float3 &pos_2 = positions[vert_2];
    const float3 pos = bke::attribute_math::mix3(bary_weights, pos_0, pos_1, pos_2);

    const float3 &tangent_pos_0 = tangent_positions[vert_0];
    const float3 &tangent_pos_1 = tangent_positions[vert_1];
    const float3 tangent_reference_dir = tangent_pos_1 - tangent_pos_0;

    /* Compute first local tangent based on the (potentially smoothed) normal and the tangent
     * reference. */
    const float3 tangent_x = math::normalize(math::cross(normal, tangent_reference_dir));

    /* The second tangent defined by the normal and first tangent. */
    const float3 tangent_y = math::normalize(math::cross(normal, tangent_x));

    /* Construct rotation matrix that encodes the orientation of the old surface position. */
    const float3x3 rotation(tangent_x, tangent_y, normal);

    float4x4 transform = math::from_origin_transform<float4x4>(float4x4(rotation), pos);

    /* Change the basis of the transformation so to that it can be applied in the local space of
     * the curves. */
    surface_transforms[curve_i] = surface_to_curves * transform * curves_to_surface;
  });

  return surface_transforms;
}

#if 0  // UNUSED for now, all these fields is hard ...
/* Sample an attribute on the curve domain using a point index.
 * This is a combination of "Evaluate on Domain" (mapping from curve domain to point domain) and
 * "Sample Index" (get the transform from curves to constraint points).
 *
 * TODO The SampleIndexFunction class isn't publicly accessible, otherwise this might be
 * constructed using a generic field operation instead of a customized multifunction.
 */
class SampleCurveByPointFunction : public mf::MultiFunction {
  GeometrySet src_geometry_;
  Array<int> point_to_curve_map_;
  GField src_field_;

  mf::Signature signature_;

  std::optional<bke::CurvesFieldContext> field_context_;
  std::unique_ptr<FieldEvaluator> evaluator_;
  const GVArray *src_data_ = nullptr;

 public:
  SampleCurveByPointFunction(GeometrySet geometry, GField src_field)
      : src_geometry_(std::move(geometry)), src_field_(std::move(src_field))
  {
    src_geometry_.ensure_owns_direct_data();

    mf::SignatureBuilder builder{"Sample Index", signature_};
    builder.single_input<int>("Point Index");
    builder.single_output("Value", src_field_.cpp_type());
    this->set_signature(&signature_);

    this->evaluate_field();
  }

  void evaluate_field()
  {
    if (!src_geometry_.has_curves()) {
      return;
    }
    const bke::CurvesGeometry &curves = src_geometry_.get_curves()->geometry.wrap();
    const int curves_num = curves.curves_num();
    if (curves_num == 0) {
      return;
    }

    point_to_curve_map_ = curves.point_to_curve_map();
    field_context_.emplace(bke::CurvesFieldContext(curves, AttrDomain::Curve));
    evaluator_ = std::make_unique<FieldEvaluator>(*field_context_, curves_num);
    evaluator_->add(src_field_);
    evaluator_->evaluate();
    src_data_ = &evaluator_->get_evaluated(0);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<int> &indices = params.readonly_single_input<int>(0, "Point Index");
    const IndexRange points = point_to_curve_map_.index_range();
    GMutableSpan dst = params.uninitialized_single_output(1, "Value");

    const CPPType &type = dst.type();
    if (src_data_ == nullptr) {
      type.value_initialize_indices(dst.data(), mask);
      return;
    }

    bke::attribute_math::convert_to_static_type(type, [&](auto dummy) {
      using T = decltype(dummy);
      VArray<T> typed_src = src_data_->typed<T>();
      MutableSpan<T> typed_dst = dst->typed<T>();
      devirtualize_varray2(typed_src, indices, [&](const auto src, const auto indices) {
        mask.foreach_index(GrainSize(4096), [&](const int i) {
          const int point_index = indices[i];
          /* Note: indices in point_to_curve_map_ are always valid. */
          typed_dst[i] = (points.contains(point_index)) ? src[point_to_curve_map_[point_index]] :
                                                          0;
        });
      });
    });
  }
};
#endif

// meh ...
#if 0
/* Array of surface transforms for unilateral curve constraints. */
static VArray<float4x4> curve_constraint_surface_transforms(
    bke::PointCloudComponent &constraints,
    const bke::CurvesGeometry &curves,
    const Span<float4x4> curve_surface_transforms)
{
  const int points_num = constraints.attribute_domain_size(AttrDomain::Point);
  MutableAttributeAccessor constraint_attributes = *constraints.attributes_for_write();
  Array point_to_curve_map = curves.point_to_curve_map();
  IndexRange points = point_to_curve_map.index_range();

  Array<int> target_point(points_num);
  constraint_attributes.lookup<int>(target_point1_attr, AttrDomain::Point)
      .varray.materialize_to_uninitialized(target_point);

  return VArray<float4x4>::ForFunc(points_num, [=](const int index) {
    const int target_point_i = target_point[index];
    if (!points.contains(target_point_i)) {
      return float4x4::identity();
    }
    const int target_curve_i = point_to_curve_map[target_point_i];
    return curve_surface_transforms[target_curve_i];
  });
}
#endif

/* Utility for different variations of functions that sample curve surface transforms.
 * TODO This should ultimately be field-based, but for now just avoids redundant code. */
template<typename Fn>
static void foreach_root_surface_transform(bke::PointCloudComponent &constraints,
                                           const Field<bool> selection_field,
                                           const bke::CurvesGeometry &curves,
                                           const Span<float4x4> curve_surface_transforms,
                                           Fn fn)
{
  if (!constraints.has_pointcloud()) {
    return;
  }

  MutableAttributeAccessor constraint_attributes = *constraints.attributes_for_write();
  const Array point_to_curve_map = curves.point_to_curve_map();
  const VArraySpan<int> target_point = *constraint_attributes.lookup<int>(target_point1_attr);

  /* Evaluate point selection. */
  bke::PointCloudFieldContext field_context(*constraints.get_for_write());
  FieldEvaluator field_evaluator(field_context,
                                 constraints.attribute_domain_size(AttrDomain::Point));
  field_evaluator.set_selection(selection_field);
  field_evaluator.evaluate();
  const IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();

  selection.foreach_index(GrainSize(256), [&](const int constraint_i) {
    const int target_point_i = target_point[constraint_i];
    if (!curves.points_range().contains(target_point_i)) {
      return;
    }
    const int target_curve_i = point_to_curve_map[target_point_i];
    const float4x4 &surface_transform = curve_surface_transforms[target_curve_i];
    fn(constraint_i, target_point_i, surface_transform);
  });
}

static std::optional<Error> capture_root_surface_position(
    bke::PointCloudComponent &constraints,
    const Field<bool> selection_field,
    const bke::CurvesGeometry &curves,
    const Span<float4x4> curve_surface_transforms)
{
  MutableAttributeAccessor constraint_attributes = *constraints.attributes_for_write();
  const VArraySpan curve_positions = *curves.attributes().lookup_or_default<float3>(
      hairsim::attributes::position, AttrDomain::Point, float3(0.0f));
  SpanAttributeWriter surface_position_writer =
      constraint_attributes.lookup_or_add_for_write_span<float3>(surface_position_attr,
                                                                 AttrDomain::Point);
  foreach_root_surface_transform(
      constraints,
      selection_field,
      curves,
      curve_surface_transforms,
      [&](const int constraint_i, const int target_point_i, const float4x4 &surface_transform) {
        surface_position_writer.span[constraint_i] = math::transform_point(
            math::invert(surface_transform), curve_positions[target_point_i]);
      });
  surface_position_writer.finish();
  return std::nullopt;
}

static std::optional<Error> capture_root_surface_rotation(
    bke::PointCloudComponent &constraints,
    const Field<bool> selection_field,
    const bke::CurvesGeometry &curves,
    const Span<float4x4> curve_surface_transforms)
{
  MutableAttributeAccessor constraint_attributes = *constraints.attributes_for_write();
  const VArraySpan curve_rotations = *curves.attributes().lookup_or_default<math::Quaternion>(
      hairsim::attributes::rotation, AttrDomain::Point, math::Quaternion::identity());
  SpanAttributeWriter surface_rotation_writer =
      constraint_attributes.lookup_or_add_for_write_span<math::Quaternion>(surface_rotation_attr,
                                                                           AttrDomain::Point);
  foreach_root_surface_transform(
      constraints,
      selection_field,
      curves,
      curve_surface_transforms,
      [&](const int constraint_i, const int target_point_i, const float4x4 &surface_transform) {
        surface_rotation_writer.span[constraint_i] = math::to_quaternion(
                                                         math::invert(surface_transform)) *
                                                     curve_rotations[target_point_i];
      });
  surface_rotation_writer.finish();
  return std::nullopt;
}

static std::optional<Error> apply_root_surface_position(
    bke::PointCloudComponent &constraints,
    const Field<bool> selection_field,
    const bke::CurvesGeometry &curves,
    const Span<float4x4> curve_surface_transforms)
{
  MutableAttributeAccessor constraint_attributes = *constraints.attributes_for_write();
  const VArraySpan surface_positions = *constraint_attributes.lookup_or_default<float3>(
      surface_position_attr, AttrDomain::Point, float3(0.0f));
  SpanAttributeWriter goal_position_writer =
      constraint_attributes.lookup_or_add_for_write_span<float3>(goal_position_attr,
                                                                 AttrDomain::Point);
  foreach_root_surface_transform(constraints,
                                 selection_field,
                                 curves,
                                 curve_surface_transforms,
                                 [&](const int constraint_i,
                                     const int /*target_point_i*/,
                                     const float4x4 &surface_transform) {
                                   goal_position_writer.span[constraint_i] = math::transform_point(
                                       surface_transform, surface_positions[constraint_i]);
                                 });
  goal_position_writer.finish();
  return std::nullopt;
}

static std::optional<Error> apply_root_surface_rotation(
    bke::PointCloudComponent &constraints,
    const Field<bool> selection_field,
    const bke::CurvesGeometry &curves,
    const Span<float4x4> curve_surface_transforms)
{
  MutableAttributeAccessor constraint_attributes = *constraints.attributes_for_write();
  const VArraySpan surface_rotations = *constraint_attributes.lookup_or_default<math::Quaternion>(
      surface_rotation_attr, AttrDomain::Point, math::Quaternion::identity());
  SpanAttributeWriter goal_rotation_writer =
      constraint_attributes.lookup_or_add_for_write_span<math::Quaternion>(goal_rotation_attr,
                                                                           AttrDomain::Point);
  foreach_root_surface_transform(
      constraints,
      selection_field,
      curves,
      curve_surface_transforms,
      [&](const int constraint_i,
          const int /*target_point_i*/,
          const float4x4 &surface_transform) {
        goal_rotation_writer.span[constraint_i] = math::to_quaternion(surface_transform) *
                                                  surface_rotations[constraint_i];
      });
  goal_rotation_writer.finish();
  return std::nullopt;
}

class LazyFunctionForRootConstraintUpdate : public LazyFunctionForClosure {
 private:
  int input_geometry_index_;
  int input_position_index_, input_rotation_index_;
  int output_position_index_, output_rotation_index_;

 public:
  LazyFunctionForRootConstraintUpdate(const int32_t node_identifier, const char *debug_name)
      : LazyFunctionForClosure(node_identifier, debug_name)
  {
    input_geometry_index_ = add_input("Geometry", SOCK_GEOMETRY);
    input_position_index_ = add_input("Position Constraints", SOCK_GEOMETRY);
    input_rotation_index_ = add_input("Rotation Constraints", SOCK_GEOMETRY);
    output_position_index_ = add_output("Position Constraints", SOCK_GEOMETRY);
    output_rotation_index_ = add_output("Rotation Constraints", SOCK_GEOMETRY);
  }

  void execute_impl(lf::Params &params, const lf::Context &context) const override
  {
    // const ScopedNodeTimer node_timer{context, node_};

    GeoNodesUserData *user_data = dynamic_cast<GeoNodesUserData *>(context.user_data);
    BLI_assert(user_data != nullptr);
    auto error_message_add = [&](const NodeWarningType type, const StringRef message) {
      GeoNodesLocalUserData *local_user_data = static_cast<GeoNodesLocalUserData *>(
          context.local_user_data);
      if (geo_eval_log::GeoTreeLogger *tree_logger = local_user_data->try_get_tree_logger(
              *user_data))
      {
        tree_logger->node_warnings.append(
            *tree_logger->allocator,
            {node_identifier_, {type, tree_logger->allocator->copy_string(message)}});
      }
    };

    GeometrySet position_constraints = params.get_input<GeometrySet>(input_position_index_);
    GeometrySet rotation_constraints = params.get_input<GeometrySet>(input_rotation_index_);
    auto pass_through_input = [&]() {
      params.set_output(output_position_index_, std::move(position_constraints));
      params.set_output(output_rotation_index_, std::move(rotation_constraints));
    };

    GeometrySet curves_geometry = params.get_input<GeometrySet>(input_geometry_index_);
    if (!curves_geometry.has_curves()) {
      pass_through_input();
      return;
    }
    bke::CurvesGeometry &curves = curves_geometry.get_curves_for_write()->geometry.wrap();
    if (curves.is_empty()) {
      pass_through_input();
      return;
    }
    if (curves.surface_uv_coords().is_empty()) {
      pass_through_input();
      error_message_add(NodeWarningType::Error, TIP_("Curves are not attached to any UV map"));
      return;
    }
    const VArraySpan surface_uv_coords = *curves.attributes().lookup_or_default<float2>(
        "surface_uv_coordinate", AttrDomain::Curve, float2(0));

    /* Find attachment points on the mesh. */
    const Object *self_ob_eval = user_data->call_data->self_object();
    HairSurfaceData surface_data;
    if (auto error = surface_data.init_from_object(self_ob_eval, true)) {
      pass_through_input();
      error_message_add(error->type, error->message);
      return;
    }

    const Array<float4x4> curve_surface_transforms = compute_curve_surface_transforms(
        curves, curves.curves_range(), surface_data);

    if (position_constraints.has_pointcloud()) {
      PointCloudComponent &position_points =
          position_constraints.get_component_for_write<PointCloudComponent>();
      apply_root_surface_position(position_points,
                                  field_constants::constant_field<bool>(true),
                                  curves,
                                  curve_surface_transforms);
    }
    if (rotation_constraints.has_pointcloud()) {
      PointCloudComponent &rotation_points =
          rotation_constraints.get_component_for_write<PointCloudComponent>();
      apply_root_surface_rotation(rotation_points,
                                  field_constants::constant_field<bool>(true),
                                  curves,
                                  curve_surface_transforms);
    }

    params.set_output(output_position_index_, std::move(position_constraints));
    params.set_output(output_rotation_index_, std::move(rotation_constraints));
  }
};

static ClosurePtr create_curve_constraint_update_closure(ResourceScope &scope,
                                                         const int32_t node_identifier)
{
  auto &body_fn = scope.construct<LazyFunctionForCurveConstraintUpdate>(
      node_identifier, "Curve Constraint Update Closure");
  return create_closure_for_lazy_function(body_fn);
}

static ClosurePtr create_root_constraint_update_closure(ResourceScope &scope,
                                                        const int32_t node_identifier)
{
  auto &body_fn = scope.construct<LazyFunctionForRootConstraintUpdate>(
      node_identifier, "Root Constraint Update Closure");
  return create_closure_for_lazy_function(body_fn);
}

/**
 * The behavior configures various details of the hair simulation.
 * It is initialized using an input bundle, but has a default implementation
 * for each part that should provide reasonable behavior without user changes.
 * Parts of the bundle can be modified without affecting the other behaviors.
 * Each item is identified by name.
 *
 * "Gravity": Single vector defining the direction of gravity in the simulation.
 *
 * "Force": Vector field of external forces acting on points. Force is defined in object space.
 * "Torque": Vector field of external torque acting on curve segments. Torque is defined in
 * local space of a curve segment.
 *
 * "Material" Bundle of parameters defining the physical properties of hair.
 *   Hair material properties can be defined in several ways:
 *   - "Density" and radius: Hair properties are calculated based on a model of cylindrical
 * rods around the center line. This is the default method if no other attributes are defined.
 *     Radius attribute must be defined on curves, otherwise a default radius is used.
 *   - "Mass" and "Inertia": If both of these fields are defined they explicitly define the
 *     material properties of hair curve points and segments.
 *     This is an advanced method that is not recommended for most users.
 *   TODO: Document how stiffness and damping are calculated based on Young's modulus for
 *     cylindrical rods.
 *
 *
 */
struct Behavior {
  float3 gravity = float3(0, 0, -9.81f);

  Field<float3> force = field_constants::zero_vector();
  Field<float3> torque = field_constants::zero_vector();

  struct {
    Field<float> density = field_constants::constant_field<float>(1000.0f);
    /* Mass is calculated from density and radius by default, but can be defined explicitly. */
    Field<float> mass = {};
    Field<float3> inertia = {};
  } material;

  struct {
    Field<float> stretch_compliance = field_constants::constant_field<float>(0.0f);
    Field<float> stretch_damping = field_constants::constant_field<float>(0.0f);
    Field<float3> bend_compliance = field_constants::constant_field<float3>(float3(0.0f));
    Field<float> bend_damping = field_constants::constant_field<float>(0.0f);

    ClosurePtr update;
  } curve_constraints;

  struct {
    Field<float3> bend_compliance = field_constants::constant_field<float3>(float3(0.0f));
    Field<float> bend_damping = field_constants::constant_field<float>(0.0f);

    ClosurePtr update;
  } root_constraints;

  /* Constructor requires a resource scope and a node identifier to create default closures. */
  Behavior(ResourceScope &scope, const int32_t node_identifier)
  {
    curve_constraints.update = create_curve_constraint_update_closure(scope, node_identifier);
    root_constraints.update = create_root_constraint_update_closure(scope, node_identifier);
  }
};

static Behavior separate_behavior_bundle(const BundlePtr &bundle,
                                         ResourceScope &scope,
                                         const int32_t node_identifier)
{
  Behavior behavior(scope, node_identifier);

  get_from_bundle(bundle, "Gravity", behavior.gravity);

  get_from_bundle(bundle, "Force", behavior.force);
  get_from_bundle(bundle, "Torque", behavior.torque);

  if (auto material = get_from_bundle<BundlePtr>(bundle, "Material")) {
    get_from_bundle(*material, "Mass", behavior.material.mass);
    get_from_bundle(*material, "Inertia", behavior.material.inertia);
    get_from_bundle(*material, "Density", behavior.material.density);
  }

  if (auto curve_constraints = get_from_bundle<BundlePtr>(bundle, "Curve Constraints")) {
    get_from_bundle(
        *curve_constraints, "Stretch Compliance", behavior.curve_constraints.stretch_compliance);
    get_from_bundle(
        *curve_constraints, "Stretch Damping", behavior.curve_constraints.stretch_damping);
    get_from_bundle(
        *curve_constraints, "Bend Compliance", behavior.curve_constraints.bend_compliance);
    get_from_bundle(*curve_constraints, "Bend Damping", behavior.curve_constraints.bend_damping);

    get_from_bundle(*curve_constraints, "Update", behavior.curve_constraints.update);
  }

  if (auto root_constraints = get_from_bundle<BundlePtr>(bundle, "Root Constraints")) {
    get_from_bundle(
        *root_constraints, "Bend Compliance", behavior.root_constraints.bend_compliance);
    get_from_bundle(*root_constraints, "Bend Damping", behavior.root_constraints.bend_damping);

    get_from_bundle(*root_constraints, "Update", behavior.root_constraints.update);
  }

  return behavior;
}

using ErrorFn = std::function<void(NodeWarningType type, const StringRef message)>;

/* Compute a rotation aligning the curve tangent with the Z axis and curve normal with X axis. */
static void store_initial_curve_rotation(CurveComponent &hair_component)
{
  /* Use the segment direction here rather than the curve tangent, since the Z axis of the rotation
   * should be perfectly aligned with the segment.*/
  Field<float3> tangent = hairsim::field_ops::curve_segment(hairsim::field_inputs::position());
  /* Arguments "legacy normals" and "true normals" are irrelevant for curves. */
  Field<float3> normal(std::make_shared<bke::NormalFieldInput>(false, false));

  auto rotation_fn = mf::build::SI2_SO<float3, float3, math::Quaternion>(
      "Segment Rotation", [](const float3 &tangent, const float3 &normal) -> math::Quaternion {
        const float3 z_axis = math::normalize(tangent);
        const float3 y_axis = math::normalize(math::cross(z_axis, normal));
        const float3 x_axis = math::cross(y_axis, z_axis);
        const float3x3 rotation(x_axis, y_axis, z_axis);
        return math::to_quaternion(rotation);
      });
  Field<math::Quaternion> rotation_field = {
      FieldOperation::Create(rotation_fn, {tangent, normal})};
  bke::try_capture_field_on_geometry(
      hair_component, hairsim::attributes::rotation, AttrDomain::Point, rotation_field);
}

static bool store_hair_rest_shape(GeometryComponent &hair_component)
{
  return bke::try_capture_fields_on_geometry(
      hair_component,
      {rest_position_attr, rest_rotation_attr},
      AttrDomain::Point,
      {hairsim::field_inputs::position(), hairsim::field_inputs::rotation()});
}

static bool try_capture_mass_attributes(GeometryComponent &component,
                                        const Field<bool> &selection_field,
                                        const Field<float> &mass_field,
                                        const Field<float3> &inertia_field)
{
  const Field<float> inv_mass_field = hairsim::field_ops::inverse_mass(mass_field);
  const Field<float3> inv_inertia_field = hairsim::field_ops::inverse_inertia(inertia_field);

  if (!bke::try_capture_fields_on_geometry(
          component,
          {hairsim::attributes::mass,
           hairsim::attributes::inertia,
           hairsim::attributes::inv_mass,
           hairsim::attributes::inv_inertia},
          bke::AttrDomain::Point,
          selection_field,
          {mass_field, inertia_field, inv_mass_field, inv_inertia_field}))
  {
    return false;
  }

  return true;
}

static bool try_init_hair_from_mass(GeometryComponent &component,
                                    const Field<bool> &selection_field,
                                    const Field<float> &mass_field,
                                    const Field<float3> &inertia_field,
                                    ErrorFn error_fn)
{
  /* Use a different method not all necessary fields are defined. */
  if (!mass_field || !inertia_field) {
    if (mass_field) {
      error_fn(NodeWarningType::Warning, "Incomplete material behavior, \"Mass\" field ignored");
    }
    if (inertia_field) {
      error_fn(NodeWarningType::Warning,
               "Incomplete material behavior, \"Inertia\" field ignored");
    }
    return false;
  }

  if (!try_capture_mass_attributes(component, selection_field, mass_field, inertia_field)) {
    return false;
  }

  return true;
}

static bool try_init_hair_from_density(GeometryComponent &component,
                                       const Field<bool> &selection_field,
                                       const Field<float> &density_field,
                                       const Field<float> &radius_field,
                                       ErrorFn /*error_fn*/)
{
  /* Use a different method not all necessary fields are defined. */
  if (!density_field) {
    return false;
  }

  const Field<float> cross_section_field = hairsim::field_ops::curve_cross_section(radius_field);
  const Field<float3> area_moment_field = hairsim::field_ops::curve_area_moment(radius_field);
  if (!bke::try_capture_fields_on_geometry(component,
                                           {cross_section_attr, area_moment_attr},
                                           bke::AttrDomain::Point,
                                           selection_field,
                                           {cross_section_field, area_moment_field}))
  {
    return false;
  }

  const Field<float> material_length_field = field_inputs::material_length();
  const Field<float> point_mass_field = hairsim::field_ops::curve_point_mass(
      material_length_field,
      AttributeFieldInput::Create<float>(cross_section_attr),
      density_field);
  const Field<float3> segment_inertia_field = hairsim::field_ops::curve_segment_inertia(
      material_length_field, field_inputs::radius(), density_field);

  if (!try_capture_mass_attributes(
          component, selection_field, point_mass_field, segment_inertia_field))
  {
    return false;
  }

  return true;
}

/* Capture hair attributes for mass, moments of inertia, rod stiffness and damping. */
static bool init_hair_physics(GeometryComponent &component,
                              const Field<bool> &selection_field,
                              const Behavior &behavior,
                              ErrorFn error_fn)
{
  /* Compute segment length. */
  {
    const Field<float3> position_field = hairsim::field_inputs::position();
    const Field<float> material_length_field = hairsim::field_ops::staggered_curve_segment_length(
        position_field);
    if (!bke::try_capture_field_on_geometry(component,
                                            material_length_attr,
                                            bke::AttrDomain::Point,
                                            selection_field,
                                            material_length_field))
    {
      return false;
    }
  }

  if (try_init_hair_from_mass(
          component, selection_field, behavior.material.mass, behavior.material.inertia, error_fn))
  {
    return true;
  }
  else if (try_init_hair_from_density(component,
                                      selection_field,
                                      behavior.material.density,
                                      field_inputs::radius(),
                                      error_fn))
  {
    return true;
  }
  else {
    /* Fall back on default values with a warning. */
    error_fn(NodeWarningType::Warning, "Missing density and radius");
    return try_init_hair_from_density(component,
                                      selection_field,
                                      field_constants::constant_field<float>(1000.0f),
                                      field_constants::constant_field<float>(0.01f),
                                      error_fn);
  }
}

/* Create internal stretch/shear and bending constraints. */
static void generate_elastic_rod_constraints(BundlePtr &bundle,
                                             const CurveComponent &component,
                                             const Field<bool> selection_field,
                                             const Behavior &behavior,
                                             GeoNodesUserData * /*user_data*/)
{
  GeometrySet stretch_constraints =
      geometry::hair_constraints::create_stretch_shear_constraints_from_curves(
          component,
          selection_field,
          behavior.curve_constraints.stretch_compliance,
          behavior.curve_constraints.stretch_damping,
          field_inputs::rest_position());
  GeometrySet bending_constraints =
      geometry::hair_constraints::create_bend_twist_constraints_from_curves(
          component,
          selection_field,
          behavior.curve_constraints.bend_compliance,
          behavior.curve_constraints.bend_damping,
          field_inputs::rest_rotation());

  hair_constraints::set_constraints(bundle, ConstraintType::StretchShear, stretch_constraints);
  hair_constraints::set_constraints(bundle, ConstraintType::BendTwist, bending_constraints);
}

static std::optional<Error> try_capture_root_constraint_offset(const bke::CurvesGeometry &curves,
                                                               GeometrySet &position_constraints,
                                                               GeometrySet &rotation_constraints,
                                                               GeoNodesUserData *user_data)
{
  // auto error_message_add = [&](const NodeWarningType type, const StringRef message) {
  //   GeoNodesLocalUserData *local_user_data = static_cast<GeoNodesLocalUserData *>(
  //       context.local_user_data);
  //   if (geo_eval_log::GeoTreeLogger *tree_logger = local_user_data->try_get_tree_logger(
  //           *user_data))
  //   {
  //     tree_logger->node_warnings.append(
  //         *tree_logger->allocator,
  //         {node_identifier_, {type, tree_logger->allocator->copy_string(message)}});
  //   }
  // };

  /* Find attachment points on the mesh. */
  const Object *self_ob_eval = user_data->call_data->self_object();
  HairSurfaceData surface_data;
  if (auto error = surface_data.init_from_object(self_ob_eval, true)) {
    // error_message_add(error->type, error->message);
    return error;
  }

  Array<float4x4> curve_surface_transforms = compute_curve_surface_transforms(
      curves, curves.curves_range(), surface_data);

  if (position_constraints.has_pointcloud()) {
    PointCloudComponent &position_points =
        position_constraints.get_component_for_write<PointCloudComponent>();
    capture_root_surface_position(position_points,
                                  field_constants::constant_field<bool>(true),
                                  curves,
                                  curve_surface_transforms);
  }
  if (rotation_constraints.has_pointcloud()) {
    PointCloudComponent &rotation_points =
        rotation_constraints.get_component_for_write<PointCloudComponent>();
    capture_root_surface_rotation(rotation_points,
                                  field_constants::constant_field<bool>(true),
                                  curves,
                                  curve_surface_transforms);
  }

  return std::nullopt;
}

/* Create root attachment constraints. */
static void generate_root_attachment_constraints(BundlePtr &bundle,
                                                 const GeometryComponent &component,
                                                 const Field<bool> selection_field,
                                                 const Behavior &behavior,
                                                 GeoNodesUserData *user_data)
{
  static const auto root_selection_fn = fn::multi_function::build::SI2_SO<bool, bool, bool>(
      "Root Selection", [](const bool selected, const bool is_start_point) -> bool {
        return selected && is_start_point;
      });
  Field<bool> root_selection{FieldOperation::Create(
      root_selection_fn, {selection_field, hairsim::field_inputs::is_curve_start_point()})};

  GeometrySet position_constraints =
      geometry::hair_constraints::create_position_goal_constraints_from_points(
          component,
          root_selection,
          field_constants::constant_field<float>(0.0f),
          field_constants::constant_field<float>(0.0f));
  GeometrySet rotation_constraints =
      geometry::hair_constraints::create_rotation_goal_constraints_from_points(
          component,
          root_selection,
          behavior.root_constraints.bend_compliance,
          behavior.root_constraints.bend_damping);

  if (component.type() == GeometryComponent::Type::Curve) {
    const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
    try_capture_root_constraint_offset(curve_component.get()->geometry.wrap(),
                                       position_constraints,
                                       rotation_constraints,
                                       user_data);
  }

  hair_constraints::set_constraints(bundle, ConstraintType::PositionGoal, position_constraints);
  hair_constraints::set_constraints(bundle, ConstraintType::RotationGoal, rotation_constraints);
}

static void update_constraints(BundlePtr &bundle,
                               const GeometrySet &hair_geometry,
                               const Behavior &behavior,
                               GeoNodesUserData *user_data)
{
  static const bke::bNodeSocketType *stype_geometry = bke::node_socket_type_find_static(
      SOCK_GEOMETRY);

  if (!bundle) {
    return;
  }
  if (bundle->is_mutable()) {
    bundle->tag_ensured_mutable();
  }
  else {
    bundle = BundlePtr(MEM_new<Bundle>(__func__, *bundle));
  }

  if (behavior.curve_constraints.update) {
    GeometrySet input_geometry = hair_geometry;
    GeometrySet stretch_constraints = hair_constraints::lookup_constraints(
        *bundle, ConstraintType::StretchShear);
    GeometrySet bend_constraints = hair_constraints::lookup_constraints(*bundle,
                                                                        ConstraintType::BendTwist);
    /* Uninitialized memory for output values. */
    GeometrySet out_stretch_constraints, out_bend_constraints;
    std::destroy_at(&out_stretch_constraints);
    std::destroy_at(&out_bend_constraints);

    ClosureEagerEvalParams params;
    params.inputs.append({SocketInterfaceKey("Geometry"), stype_geometry, &input_geometry});
    params.inputs.append(
        {SocketInterfaceKey("Stretch Constraints"), stype_geometry, &stretch_constraints});
    params.inputs.append(
        {SocketInterfaceKey("Bend Constraints"), stype_geometry, &bend_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Stretch Constraints"), stype_geometry, &out_stretch_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Bend Constraints"), stype_geometry, &out_bend_constraints});
    params.user_data = user_data;
    evaluate_closure_eagerly(*behavior.curve_constraints.update, params);

    hair_constraints::set_constraints(
        bundle, ConstraintType::StretchShear, out_stretch_constraints);
    hair_constraints::set_constraints(bundle, ConstraintType::BendTwist, out_bend_constraints);
  }

  if (behavior.root_constraints.update) {
    GeometrySet input_geometry = hair_geometry;
    GeometrySet position_constraints = hair_constraints::lookup_constraints(
        *bundle, ConstraintType::PositionGoal);
    GeometrySet rotation_constraints = hair_constraints::lookup_constraints(
        *bundle, ConstraintType::RotationGoal);
    /* Uninitialized memory for output values. */
    GeometrySet out_position_constraints, out_rotation_constraints;
    std::destroy_at(&out_position_constraints);
    std::destroy_at(&out_rotation_constraints);

    ClosureEagerEvalParams params;
    params.inputs.append({SocketInterfaceKey("Geometry"), stype_geometry, &input_geometry});
    params.inputs.append(
        {SocketInterfaceKey("Position Constraints"), stype_geometry, &position_constraints});
    params.inputs.append(
        {SocketInterfaceKey("Rotation Constraints"), stype_geometry, &rotation_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Position Constraints"), stype_geometry, &out_position_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Rotation Constraints"), stype_geometry, &out_rotation_constraints});
    params.user_data = user_data;
    evaluate_closure_eagerly(*behavior.root_constraints.update, params);

    hair_constraints::set_constraints(
        bundle, ConstraintType::PositionGoal, out_position_constraints);
    hair_constraints::set_constraints(
        bundle, ConstraintType::RotationGoal, out_rotation_constraints);
  }
}

/* Capture motions state attribute for later velocity estimation. */
static bool capture_motion_state(GeometryComponent &component, const Field<bool> &selection_field)
{
  return bke::try_capture_fields_on_geometry(
      component,
      {old_position_attr, old_rotation_attr},
      AttrDomain::Point,
      selection_field,
      {hairsim::field_inputs::position(), hairsim::field_inputs::rotation()});
}

static void zero_init_solver(MutableSpan<ConstraintEvalData> constraint_data)
{
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }
    if (data.type->init_step) {
      data.type->init_step(*data.geometry);
    }
  }
}

static void warm_start_solver(const ConstraintEvalParams &eval_params,
                              MutableSpan<ConstraintEvalData> constraint_data)
{
  for (ConstraintEvalData &data : constraint_data) {
    if (!data.geometry) {
      continue;
    }
    /* TODO */
    eval_params.error_message_add("Warm starting not yet implemented");
    if (data.type->init_step) {
      data.type->init_step(*data.geometry);
    }
  }
}

static void estimate_velocity(ConstraintEvalParams &params,
                              Array<float3> &orig_velocities,
                              Array<float3> &orig_angular_velocities,
                              ConstraintVariables &vars)
{
  const Span<float3> old_positions = params.old_positions;
  const Span<math::Quaternion> old_rotations = params.old_rotations;
  const Span<float3> positions = vars.positions;
  const Span<math::Quaternion> rotations = vars.rotations;
  MutableSpan<float3> velocities = vars.velocities;
  MutableSpan<float3> angular_velocities = vars.angular_velocities;
  const IndexMask positions_mask = vars.positions.index_range();
  const IndexMask rotations_mask = vars.rotations.index_range();
  const float inv_dt = params.inv_delta_time;

  orig_velocities = vars.velocities;
  orig_angular_velocities = vars.angular_velocities;
  params.orig_velocities = orig_velocities;
  params.orig_angular_velocities = orig_angular_velocities;

  positions_mask.foreach_index(GrainSize(1024), [&](const int index) {
    velocities[index] = inv_dt * (positions[index] - old_positions[index]);
  });
  rotations_mask.foreach_index(GrainSize(1024), [&](const int index) {
    angular_velocities[index] =
        2.0f * inv_dt *
        (math::invert_normalized(old_rotations[index]) * rotations[index]).imaginary_part();
  });
}

static void do_position_constraints_iteration(const ConstraintEvalParams &eval_params,
                                              MutableSpan<ConstraintEvalData> constraint_data,
                                              ConstraintVariables &variables)
{
  IndexMaskMemory memory;

  Array<VariableIndexArrays> indices_by_type(constraint_data.size());
  geometry::hair_solver::read_constraint_topology(constraint_data, indices_by_type);

  for (const int constraint_i : constraint_data.index_range()) {
    ConstraintEvalData &data = constraint_data[constraint_i];
    const VariableIndexArrays &indices = indices_by_type[constraint_i];
    if (!data.geometry) {
      continue;
    }

    /* Solve in consistent order by using the sorted index set. */
    for (const IndexMask &group_mask : data.group_masks) {
      apply_gauss_seidel_positions_group(
          eval_params, *data.type, *data.geometry, group_mask, variables, indices, memory);
    }
  }
}

static void do_velocity_constraints_iteration(const ConstraintEvalParams &eval_params,
                                              MutableSpan<ConstraintEvalData> constraint_data,
                                              ConstraintVariables &variables)
{
  IndexMaskMemory memory;

  Array<VariableIndexArrays> indices_by_type(constraint_data.size());
  geometry::hair_solver::read_constraint_topology(constraint_data, indices_by_type);

  for (const int constraint_i : constraint_data.index_range()) {
    ConstraintEvalData &data = constraint_data[constraint_i];
    const VariableIndexArrays &indices = indices_by_type[constraint_i];
    if (!data.geometry) {
      continue;
    }

    /* Solve in consistent order by using the sorted index set. */
    for (const IndexMask &group_mask : data.group_masks) {
      apply_gauss_seidel_velocities_group(
          eval_params, *data.type, *data.geometry, group_mask, variables, indices, memory);
    }
  }
}

static ConstraintEvalParams extract_eval_params(GeoNodeExecParams params)
{
  // std::optional<GeometrySet> debug_steps;
  // if (params.output_is_required("Debug Steps")) {
  //   debug_steps = params.extract_input<GeometrySet>("Debug Steps");
  // }
  return ConstraintEvalParams(
      params.extract_input<float>("Delta Time"),
      [params](const StringRef message) {
        params.error_message_add(NodeWarningType::Warning, message);
      },
      false /*params.extract_input<bool>("Debug Checks")*/,
      std::nullopt /*debug_steps*/);
}

static void solve_constraints(ConstraintEvalParams &eval_params,
                              GeometryComponent &component,
                              const int iterations,
                              const MutableSpan<ConstraintEvalData> constraint_data)
{
  /* TODO warm start doesn't work properly yet. */
  const bool warm_start = false;

  const int num_points = component.attribute_domain_size(AttrDomain::Point);
  ConstraintVariables variables;
  variables.positions.reinitialize(num_points);
  variables.rotations.reinitialize(num_points);
  variables.velocities.reinitialize(num_points);
  variables.angular_velocities.reinitialize(num_points);

  const bke::GeometryFieldContext field_context{component, AttrDomain::Point};
  fn::FieldEvaluator evaluator{field_context, num_points};
  evaluator.add(hairsim::field_inputs::mass());
  evaluator.add(hairsim::field_inputs::inertia());
  evaluator.add(field_inputs::old_position());
  evaluator.add(field_inputs::old_rotation());
  evaluator.add_with_destination(hairsim::field_inputs::position(),
                                 variables.positions.as_mutable_span());
  evaluator.add_with_destination(hairsim::field_inputs::rotation(),
                                 variables.rotations.as_mutable_span());
  evaluator.add_with_destination(hairsim::field_inputs::velocity(),
                                 variables.velocities.as_mutable_span());
  evaluator.add_with_destination(hairsim::field_inputs::angular_velocity(),
                                 variables.angular_velocities.as_mutable_span());
  evaluator.evaluate();

  eval_params.masses = evaluator.get_evaluated<float>(0);
  eval_params.local_inertia = evaluator.get_evaluated<float3>(1);
  eval_params.old_positions = evaluator.get_evaluated<float3>(2);
  eval_params.old_rotations = evaluator.get_evaluated<math::Quaternion>(3);

  // eval_params.collider_transforms = collider_transforms;
  // /* XXX Transforms of the previous frame are not currently available, these are always the
  //  * same as the current frame. Eventually this will allow transfer of velocity from
  //  animated
  //  * colliders. */
  // eval_params.old_collider_transforms = eval_params.collider_transforms;

  // if (eval_params.debug_recorder) {
  //   const std::string label = fmt::format("Initialize Gauss-Seidel, ");
  //   eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
  // }

  if (warm_start) {
    warm_start_solver(eval_params, constraint_data);
  }
  else {
    zero_init_solver(constraint_data);
  }

  for ([[maybe_unused]] const int i : IndexRange(iterations)) {
    do_position_constraints_iteration(eval_params, constraint_data, variables);
  }

  Array<float3> orig_velocities;
  Array<float3> orig_angular_velocities;
  estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

  do_velocity_constraints_iteration(eval_params, constraint_data, variables);

  {
    MutableAttributeAccessor attributes = *component.attributes_for_write();
    AttributeWriter<float3> positions_writer = attributes.lookup_or_add_for_write<float3>(
        hairsim::attributes::position, AttrDomain::Point);
    AttributeWriter<math::Quaternion> rotations_writer =
        attributes.lookup_or_add_for_write<math::Quaternion>(hairsim::attributes::rotation,
                                                             AttrDomain::Point);
    AttributeWriter<float3> velocities_writer = attributes.lookup_or_add_for_write<float3>(
        hairsim::attributes::velocity, AttrDomain::Point);
    AttributeWriter<float3> angular_velocities_writer = attributes.lookup_or_add_for_write<float3>(
        hairsim::attributes::angular_velocity, AttrDomain::Point);

    BLI_assert(variables.positions.size() == num_points);
    BLI_assert(variables.rotations.size() == num_points);
    BLI_assert(variables.velocities.size() == num_points);
    BLI_assert(variables.angular_velocities.size() == num_points);

    positions_writer.varray.set_all(variables.positions);
    rotations_writer.varray.set_all(variables.rotations);
    velocities_writer.varray.set_all(variables.velocities);
    angular_velocities_writer.varray.set_all(variables.angular_velocities);

    positions_writer.finish();
    rotations_writer.finish();
    velocities_writer.finish();
    angular_velocities_writer.finish();
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const float linear_factor = 1.0f;
  const float angular_factor = 1.0f;

  const float delta_time = std::max(params.extract_input<float>("Delta Time"), 0.0f);
  const int constraint_iterations = std::max(params.extract_input<int>("Constraint Iterations"),
                                             0);

  ResourceScope scope;
  GeometrySet hair_geometry = params.extract_input<GeometrySet>("Hair");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Behavior behavior = separate_behavior_bundle(
      params.extract_input<BundlePtr>("Behavior"), scope, params.node().identifier);
  BundlePtr constraint_bundle = params.extract_input<BundlePtr>("Data");
  // GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  // Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
  //                                          colliders_geometry_set.get_instances()->transforms()
  //                                          : Span<float4x4>{};

  if (!hair_geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }
  CurveComponent &hair_curves = hair_geometry.get_component_for_write<CurveComponent>();

  ConstraintEvalParams eval_params = extract_eval_params(params);
  const bool debug_output = (eval_params.debug_recorder != nullptr);
  if (debug_output) {
    eval_params.debug_recorder->set_geometry(hair_geometry, GeometryComponent::Type::Curve);
  }

  /* Zero time step initializes the hair simulation. */
  if (delta_time == 0.0f) {
    store_initial_curve_rotation(hair_curves);
    if (!store_hair_rest_shape(hair_curves)) {
      params.error_message_add(NodeWarningType::Error, "Could not store rest shape");
    }
    init_hair_physics(
        hair_curves, selection_field, behavior, [params](NodeWarningType type, StringRef message) {
          params.error_message_add(type, message);
        });

    /* Clear any existing constraint data. */
    constraint_bundle = Bundle::create();
    generate_elastic_rod_constraints(
        constraint_bundle, hair_curves, selection_field, behavior, params.user_data());
    generate_root_attachment_constraints(
        constraint_bundle, hair_curves, selection_field, behavior, params.user_data());
  }

  update_constraints(constraint_bundle, hair_geometry, behavior, params.user_data());

  IndexMaskMemory memory;
  Vector<ConstraintEvalData> constraint_data = hair_constraints::constraint_bundle_to_eval_data(
      std::move(constraint_bundle), debug_output, memory);

  /* Store current motion state for later velocity estimation. */
  capture_motion_state(hair_curves, selection_field);

  /* Unconstrained motion. */
  hairsim::integrate_motion(hair_curves,
                            selection_field,
                            delta_time,
                            linear_factor,
                            angular_factor,
                            behavior.gravity,
                            behavior.force,
                            behavior.torque);

  solve_constraints(eval_params, hair_curves, constraint_iterations, constraint_data);

  /* Remove temporary captured attributes. */
  hair_curves.attributes_for_write()->remove(old_position_attr);
  hair_curves.attributes_for_write()->remove(old_rotation_attr);
  hair_curves.attributes_for_write()->remove(cross_section_attr);
  hair_curves.attributes_for_write()->remove(area_moment_attr);

  params.set_output("Hair", std::move(hair_geometry));
  params.set_output("Data", hair_constraints::constraint_eval_data_to_bundle(constraint_data));
}

static void node_rna(StructRNA *srna)
{
  UNUSED_VARS(srna);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeHairSimulation");
  ntype.ui_name = "Hair Simulation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_hair_simulation_cc
