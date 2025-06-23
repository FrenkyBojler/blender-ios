/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "BKE_anonymous_attribute_make.hh"
#include "BKE_curves.hh"
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

#include "GEO_hair_solver.hh"

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

static const std::string position_attr = "position";
static const std::string rotation_attr = "rotation";
static const std::string rest_position_attr = "rest_position";
static const std::string rest_rotation_attr = "rest_rotation";
static const std::string velocity_attr = "velocity";
static const std::string angular_velocity_attr = "angular_velocity";
static const std::string mass_attr = "mass";
static const std::string inv_mass_attr = "inv_mass";
static const std::string inertia_attr = "inertia";
static const std::string inv_inertia_attr = "inv_inertia";
static const std::string radius_attr = "radius";

/* XXX These should be anonymous attributes. */
static const std::string old_position_attr = ".old_position";
static const std::string old_rotation_attr = ".old_rotation";
static const std::string cross_section_attr = ".cross_section";
static const std::string area_moment_attr = ".area_moment";
/* Segment length attribute for material calculation purposes. */
static const std::string material_length_attr = ".material_length";

/* Shift point indices along the curve.
 * Indices at the start or end are clamped to the curve range. */
class ShiftedIndexOnCurveInput final : public bke::CurvesFieldInput {
 private:
  int offset_;

 public:
  ShiftedIndexOnCurveInput(const int offset)
      : bke::CurvesFieldInput(CPPType::get<int>(), "Shifted Index on Curve"), offset_(offset)
  {
  }

  GVArray get_varray_for_context(const bke::CurvesGeometry &curves,
                                 AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    if (domain != AttrDomain::Point) {
      return {};
    }

    Array<int> output(mask.min_array_size());
    const OffsetIndices points_by_curve = curves.points_by_curve();
    threading::parallel_for(curves.curves_range(), 1024, [&](IndexRange curves_range) {
      for (const int i : curves_range) {
        const IndexRange points = points_by_curve[i];
        const int start = std::max(-offset_, 0);
        const int end = std::max(offset_, 0);

        for (const int point_i : points.take_front(start)) {
          output[point_i] = points.first();
        }
        for (const int point_i : points.take_back(end)) {
          output[point_i] = points.last();
        }
        for (const int point_i : points.drop_front(start).drop_back(end)) {
          output[point_i] = point_i + offset_;
        }
      }
    });
    return VArray<int>::ForContainer(std::move(output));
  }

  std::optional<AttrDomain> preferred_domain(const bke::CurvesGeometry & /*curves*/) const final
  {
    return AttrDomain::Point;
  }
};

class IsStartPointFieldInput final : public bke::CurvesFieldInput {
 public:
  IsStartPointFieldInput() : bke::CurvesFieldInput(CPPType::get<bool>(), "Is Start Point node")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::CurvesGeometry &curves,
                                 AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    if (domain != AttrDomain::Point) {
      return {};
    }

    Array<bool> selection(mask.min_array_size(), false);
    MutableSpan<bool> selection_span = selection.as_mutable_span();
    const OffsetIndices points_by_curve = curves.points_by_curve();
    threading::parallel_for(curves.curves_range(), 1024, [&](IndexRange curves_range) {
      for (const int i : curves_range) {
        const IndexRange points = points_by_curve[i];
        if (!points.is_empty()) {
          selection_span[points.first()] = true;
        }
      }
    });

    return VArray<bool>::ForContainer(std::move(selection));
  };

  std::optional<AttrDomain> preferred_domain(const bke::CurvesGeometry & /*curves*/) const final
  {
    return AttrDomain::Point;
  }
};

class IsEndPointFieldInput final : public bke::CurvesFieldInput {
 public:
  IsEndPointFieldInput() : bke::CurvesFieldInput(CPPType::get<bool>(), "Is End Point node")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::CurvesGeometry &curves,
                                 AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    if (domain != AttrDomain::Point) {
      return {};
    }

    Array<bool> selection(mask.min_array_size(), false);
    MutableSpan<bool> selection_span = selection.as_mutable_span();
    const OffsetIndices points_by_curve = curves.points_by_curve();
    threading::parallel_for(curves.curves_range(), 1024, [&](IndexRange curves_range) {
      for (const int i : curves_range) {
        const IndexRange points = points_by_curve[i];
        if (!points.is_empty()) {
          selection_span[points.last()] = true;
        }
      }
    });

    return VArray<bool>::ForContainer(std::move(selection));
  };

  std::optional<AttrDomain> preferred_domain(const bke::CurvesGeometry & /*curves*/) const final
  {
    return AttrDomain::Point;
  }
};

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

static Field<bool> is_start_point()
{
  return Field<bool>{std::make_shared<IsStartPointFieldInput>()};
}

static Field<bool> is_end_point()
{
  return Field<bool>{std::make_shared<IsEndPointFieldInput>()};
}

static Field<float3> position()
{
  return bke::AttributeFieldInput::Create<float3>(position_attr);
}

static Field<math::Quaternion> rotation()
{
  return bke::AttributeFieldInput::Create<math::Quaternion>(rotation_attr);
}

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

static Field<float3> velocity()
{
  return bke::AttributeFieldInput::Create<float3>(velocity_attr);
}

static Field<float3> angular_velocity()
{
  return bke::AttributeFieldInput::Create<float3>(angular_velocity_attr);
}

static Field<float> radius()
{
  return bke::AttributeFieldInput::Create<float>(radius_attr);
}

static Field<float> mass()
{
  return bke::AttributeFieldInput::Create<float>(mass_attr);
}

static Field<float> inverse_mass()
{
  return bke::AttributeFieldInput::Create<float>(inv_mass_attr);
}

static Field<float3> inertia()
{
  return bke::AttributeFieldInput::Create<float3>(inertia_attr);
}

static Field<float3> inverse_inertia()
{
  return bke::AttributeFieldInput::Create<float3>(inv_inertia_attr);
}

static Field<float> material_length()
{
  return bke::AttributeFieldInput::Create<float>(material_length_attr);
}

}  // namespace field_inputs

namespace field_ops {

static GField shifted_curve_value(const GField &value_field, const int offset)
{
  Field<int> index_field{std::make_shared<ShiftedIndexOnCurveInput>(offset)};
  return GField{std::make_shared<bke::EvaluateAtIndexInput>(
      std::move(index_field), value_field, AttrDomain::Point)};
}

static Field<float> cross_section(const Field<float> radius_field)
{
  static const auto cross_section_fn = fn::multi_function::build::SI1_SO<float, float>(
      "Rod Cross Section", [](const float radius) -> float { return M_PI * radius * radius; });
  return Field<float>(fn::FieldOperation::Create(cross_section_fn, {radius_field}));
}

static Field<float3> area_moment(const Field<float> radius_field)
{
  static const auto area_moment_fn = fn::multi_function::build::SI1_SO<float, float3>(
      "Second Moment of Area", [](const float radius) -> float3 {
        const float radius_sq = radius * radius;
        return radius_sq * radius_sq * M_PI * float3(0.25f, 0.25f, 0.5f);
      });
  return Field<float3>(fn::FieldOperation::Create(area_moment_fn, {radius_field}));
}

static Field<float> segment_length(const Field<float3> &position_field)
{
  static const auto segment_length_fn = fn::multi_function::build::SI2_SO<float3, float3, float>(
      "Segment Length", [](const float3 &pt, const float3 &pt_next) -> float {
        return math::distance(pt, pt_next);
      });
  return Field<float>(fn::FieldOperation::Create(
      segment_length_fn, {position_field, field_ops::shifted_curve_value(position_field, 1)}));
}

static Field<float> average_segment_length(const Field<float3> &position_field)
{
  static const auto avg_segment_length_fn =
      fn::multi_function::build::SI4_SO<float, float, bool, bool, float>(
          "Average Segment Length",
          [](const float length_prev, const float length, const bool is_start, const bool is_end)
              -> float {
            const float weight_prev = !is_start;
            const float weight = !is_end;
            return math::safe_divide(length_prev * weight_prev + length * weight,
                                     weight_prev + weight);
          });
  Field<float> segment_length_field = segment_length(position_field);
  return Field<float>(
      fn::FieldOperation::Create(avg_segment_length_fn,
                                 {field_ops::shifted_curve_value(segment_length_field, -1),
                                  segment_length_field,
                                  field_inputs::is_start_point(),
                                  field_inputs::is_end_point()}));
}

static Field<float> point_mass(const Field<float> &segment_length_field,
                               const Field<float> &cross_section_field,
                               const Field<float> &density_field)
{
  static const auto point_mass_fn = fn::multi_function::build::SI3_SO<float, float, float, float>(
      "Point Mass",
      [](const float length, const float cross_section, const float density) -> float {
        return length * cross_section * density;
      });
  return Field<float>(fn::FieldOperation::Create(
      point_mass_fn, {segment_length_field, cross_section_field, density_field}));
}

static Field<float3> segment_inertia(const Field<float> &segment_length_field,
                                     const Field<float3> &area_moment_field,
                                     const Field<float> &density_field)
{
  static const auto segment_inertia_fn =
      fn::multi_function::build::SI3_SO<float, float3, float, float3>(
          "Segment Moment of Inertia",
          [](const float length, const float3 &area_moment, const float density) -> float3 {
            return length * area_moment * density;
          });
  return Field<float3>(fn::FieldOperation::Create(
      segment_inertia_fn, {segment_length_field, area_moment_field, density_field}));
}

static Field<float> inverse_mass(const Field<float> &mass_field)
{
  static const auto inv_mass_fn = fn::multi_function::build::SI1_SO<float, float>(
      "Inverse Mass", [](const float mass) -> float { return math::safe_rcp(mass); });
  return Field<float>(fn::FieldOperation::Create(inv_mass_fn, {mass_field}));
}

static Field<float3> inverse_inertia(const Field<float3> &inertia_field)
{
  static const auto inv_inertia_fn = fn::multi_function::build::SI1_SO<float3, float3>(
      "Inverse Moment of Inertia",
      [](const float3 &inertia) -> float3 { return math::safe_rcp(inertia); });
  return Field<float3>(fn::FieldOperation::Create(inv_inertia_fn, {inertia_field}));
}

}  // namespace field_ops

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
  int input_stretch_index_, input_bend_index_;
  int output_stretch_index_, output_bend_index_;

 public:
  LazyFunctionForCurveConstraintUpdate(const int32_t node_identifier, const char *debug_name)
      : LazyFunctionForClosure(node_identifier, debug_name)
  {
    input_stretch_index_ = add_input("Stretch Constraints", SOCK_GEOMETRY);
    input_bend_index_ = add_input("Bend Constraints", SOCK_GEOMETRY);
    output_stretch_index_ = add_output("Stretch Constraints", SOCK_GEOMETRY);
    output_bend_index_ = add_output("Bend Constraints", SOCK_GEOMETRY);
  }

  void execute_impl(lf::Params &params, const lf::Context &context) const override
  {
    // const ScopedNodeTimer node_timer{context, node_};

    GeoNodesUserData *user_data = dynamic_cast<GeoNodesUserData *>(context.user_data);
    BLI_assert(user_data != nullptr);

    GeometrySet stretch_constraints = params.get_input<GeometrySet>(input_stretch_index_);
    GeometrySet bend_constraints = params.get_input<GeometrySet>(input_bend_index_);
    params.set_output(output_stretch_index_, std::move(stretch_constraints));
    params.set_output(output_bend_index_, std::move(bend_constraints));
  }
};

// TODO share this with node_geo_deform_curves_on_surface.cc
// TODO make it non-copyable? (prevent potential double free of allocated mesh data)
class HairSurfaceData {
 private:
  Object *surface_ob_orig_ = nullptr;
  Object *surface_ob_eval_ = nullptr;
  Mesh *surface_mesh_orig_ = nullptr;
  Mesh *surface_mesh_eval_ = nullptr;
  bool free_suface_mesh_orig_ = false;
  StringRefNull uv_map_name_;

 public:
  ~HairSurfaceData()
  {
    if (free_suface_mesh_orig_) {
      BKE_id_free(nullptr, surface_mesh_orig_);
    }
  }

  bool is_valid() const
  {
    return surface_ob_orig_ != nullptr && surface_ob_eval_ != nullptr;
  }

  operator bool() const
  {
    return is_valid();
  }

  Object &surface_ob_orig() const
  {
    return *this->surface_ob_orig_;
  }
  Object &surface_ob_eval() const
  {
    return *surface_ob_eval_;
  }
  Mesh &surface_mesh_orig() const
  {
    return *surface_mesh_orig_;
  }
  Mesh &surface_mesh_eval() const
  {
    return *surface_mesh_eval_;
  }
  StringRefNull uv_map_name() const
  {
    return uv_map_name_;
  }

  struct Error {
    NodeWarningType type;
    std::string message;
  };

  using Result = std::tuple<HairSurfaceData, std::optional<Error>>;

  static Result result_valid(HairSurfaceData &&data)
  {
    return {data, std::nullopt};
  }

  static Result result_error(const NodeWarningType type, StringRef message)
  {
    return {HairSurfaceData{}, std::make_optional<Error>({type, message})};
  }

  static Result from_object(const Object *self_ob_eval)
  {
    static const StringRefNull rest_position_name = "rest_position";

    if (self_ob_eval == nullptr || self_ob_eval->type != OB_CURVES) {
      return result_error(NodeWarningType::Error, TIP_("Node only works for curves objects"));
    }
    const Curves *self_curves_eval = static_cast<const Curves *>(self_ob_eval->data);
    if (self_curves_eval->surface_uv_map == nullptr || self_curves_eval->surface_uv_map[0] == '\0')
    {
      return result_error(NodeWarningType::Error, TIP_("Surface UV map not defined"));
    }
    /* Take surface information from self-object. */
    HairSurfaceData result;
    result.surface_ob_eval_ = self_curves_eval->surface;
    result.uv_map_name_ = self_curves_eval->surface_uv_map;

    if (result.surface_ob_eval_ == nullptr || result.surface_ob_eval_->type != OB_MESH) {
      return result_error(NodeWarningType::Error, TIP_("Curves not attached to a surface"));
    }
    result.surface_ob_orig_ = DEG_get_original(result.surface_ob_eval_);
    Mesh &surface_object_data = *static_cast<Mesh *>(result.surface_ob_orig_->data);

    if (BMEditMesh *em = surface_object_data.runtime->edit_mesh.get()) {
      result.surface_mesh_orig_ = BKE_mesh_from_bmesh_for_eval_nomain(
          em->bm, nullptr, &surface_object_data);
      result.free_suface_mesh_orig_ = true;
    }
    else {
      result.surface_mesh_orig_ = &surface_object_data;
    }
    result.surface_mesh_eval_ = BKE_modifier_get_evaluated_mesh_from_evaluated_object(
        result.surface_ob_eval_);
    if (result.surface_mesh_eval_ == nullptr) {
      return result_error(NodeWarningType::Error, TIP_("Surface has no mesh"));
    }

    BKE_mesh_wrapper_ensure_mdata(result.surface_mesh_eval_);

    const AttributeAccessor mesh_attributes_eval = result.surface_mesh_eval_->attributes();
    const AttributeAccessor mesh_attributes_orig = result.surface_mesh_orig_->attributes();

    if (!mesh_attributes_eval.contains(result.uv_map_name_)) {
      return result_error(
          NodeWarningType::Error,
          fmt::format(fmt::runtime(TIP_("Evaluated surface missing UV map: \"{}\"")),
                      result.uv_map_name_));
    }
    if (!mesh_attributes_orig.contains(result.uv_map_name_)) {
      return result_error(
          NodeWarningType::Error,
          fmt::format(fmt::runtime(TIP_("Original surface missing UV map: \"{}\"")),
                      result.uv_map_name_));
    }
    if (!mesh_attributes_eval.contains(rest_position_name)) {
      return result_error(NodeWarningType::Error,
                          TIP_("Evaluated surface missing attribute: \"rest_position\""));
    }

    return result_valid(std::move(result));
  }
};

class LazyFunctionForRootConstraintUpdate : public LazyFunctionForClosure {
 private:
  int input_position_index_, input_rotation_index_;
  int output_position_index_, output_rotation_index_;

 public:
  LazyFunctionForRootConstraintUpdate(const int32_t node_identifier, const char *debug_name)
      : LazyFunctionForClosure(node_identifier, debug_name)
  {
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

    // }; GeometrySet curves_geometry = params.extract_input<GeometrySet>("Curves"); if
    // (!curves_geometry.has_curves()) {
    //   pass_through_input();
    //   return;
    // }
    // Curves &curves_id = *curves_geometry.get_curves_for_write();
    // CurvesGeometry &curves = curves_id.geometry.wrap();
    // if (curves.surface_uv_coords().is_empty() && curves.curves_num() > 0) {
    //   pass_through_input();
    //   params.error_message_add(NodeWarningType::Error,
    //                            TIP_("Curves are not attached to any UV map"));
    //   return;
    // }
    const Object *self_ob_eval = user_data->call_data->self_object();
    auto [surface_data, surface_error] = HairSurfaceData::from_object(self_ob_eval);
    if (surface_error) {
      pass_through_input();
      error_message_add(surface_error->type, surface_error->message);
      return;
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
 * "Torque": Vector field of external torque acting on curve segments. Torque is defined in local
 *   space of a curve segment.
 *
 * "Material" Bundle of parameters defining the physical properties of hair.
 *   Hair material properties can be defined in several ways:
 *   - "Density" and radius: Hair properties are calculated based on a model of cylindrical rods
 *     around the center line. This is the default method if no other attributes are defined.
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
    Field<float> mass = field_constants::constant_field<float>(1.0f);
    Field<float3> inertia = field_constants::constant_field<float3>(float3(1.0f));
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

static bool store_hair_rest_shape(GeometryComponent &hair_component)
{
  return bke::try_capture_fields_on_geometry(hair_component,
                                             {rest_position_attr, rest_rotation_attr},
                                             AttrDomain::Point,
                                             {field_inputs::position(), field_inputs::rotation()});
}

static bool try_capture_mass_attributes(GeometryComponent &component,
                                        const Field<bool> &selection_field,
                                        const Field<float> &mass_field,
                                        const Field<float3> &inertia_field)
{
  const Field<float> inv_mass_field = field_ops::inverse_mass(mass_field);
  const Field<float3> inv_inertia_field = field_ops::inverse_inertia(inertia_field);

  if (!bke::try_capture_fields_on_geometry(
          component,
          {mass_attr, inertia_attr, inv_mass_attr, inv_inertia_attr},
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

  const Field<float> cross_section_field = field_ops::cross_section(radius_field);
  const Field<float3> area_moment_field = field_ops::area_moment(radius_field);
  if (!bke::try_capture_fields_on_geometry(component,
                                           {cross_section_attr, area_moment_attr},
                                           bke::AttrDomain::Point,
                                           selection_field,
                                           {cross_section_field, area_moment_field}))
  {
    return false;
  }

  const Field<float> material_length_field = field_inputs::material_length();
  const Field<float> point_mass_field = field_ops::point_mass(
      material_length_field,
      AttributeFieldInput::Create<float>(cross_section_attr),
      density_field);
  const Field<float3> segment_inertia_field = field_ops::segment_inertia(
      material_length_field, AttributeFieldInput::Create<float3>(area_moment_attr), density_field);

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
    const Field<float3> position_field = field_inputs::position();
    const Field<float> material_length_field = field_ops::average_segment_length(position_field);
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
                                             const Behavior &behavior)
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

/* Create root attachment constraints. */
static void generate_root_attachment_constraints(BundlePtr &bundle,
                                                 const GeometryComponent &component,
                                                 const Field<bool> selection_field,
                                                 const Behavior &behavior)
{
  static const auto root_selection_fn = fn::multi_function::build::SI2_SO<bool, bool, bool>(
      "Root Selection", [](const bool selected, const bool is_start_point) -> bool {
        return selected && is_start_point;
      });
  Field<bool> root_selection{FieldOperation::Create(
      root_selection_fn, {selection_field, field_inputs::is_start_point()})};

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

  hair_constraints::set_constraints(bundle, ConstraintType::PositionGoal, position_constraints);
  hair_constraints::set_constraints(bundle, ConstraintType::RotationGoal, rotation_constraints);
}

static void update_constraints(BundlePtr &bundle,
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
    GeometrySet stretch_constraints = hair_constraints::lookup_constraints(
        *bundle, ConstraintType::StretchShear);
    GeometrySet bend_constraints = hair_constraints::lookup_constraints(*bundle,
                                                                        ConstraintType::BendTwist);

    ClosureEagerEvalParams params;
    params.inputs.append(
        {SocketInterfaceKey("Stretch Constraints"), stype_geometry, &stretch_constraints});
    params.inputs.append(
        {SocketInterfaceKey("Bend Constraints"), stype_geometry, &bend_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Stretch Constraints"), stype_geometry, &stretch_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Bend Constraints"), stype_geometry, &bend_constraints});
    params.user_data = user_data;
    evaluate_closure_eagerly(*behavior.curve_constraints.update, params);

    hair_constraints::set_constraints(bundle, ConstraintType::StretchShear, stretch_constraints);
    hair_constraints::set_constraints(bundle, ConstraintType::BendTwist, bend_constraints);
  }

  if (behavior.root_constraints.update) {
    GeometrySet position_constraints = hair_constraints::lookup_constraints(
        *bundle, ConstraintType::PositionGoal);
    GeometrySet rotation_constraints = hair_constraints::lookup_constraints(
        *bundle, ConstraintType::RotationGoal);

    ClosureEagerEvalParams params;
    params.inputs.append(
        {SocketInterfaceKey("Position Constraints"), stype_geometry, &position_constraints});
    params.inputs.append(
        {SocketInterfaceKey("Rotation Constraints"), stype_geometry, &rotation_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Position Constraints"), stype_geometry, &position_constraints});
    params.outputs.append(
        {SocketInterfaceKey("Rotation Constraints"), stype_geometry, &rotation_constraints});
    params.user_data = user_data;
    evaluate_closure_eagerly(*behavior.root_constraints.update, params);

    hair_constraints::set_constraints(bundle, ConstraintType::PositionGoal, position_constraints);
    hair_constraints::set_constraints(bundle, ConstraintType::RotationGoal, rotation_constraints);
  }
}

/* Capture motions state attribute for later velocity estimation. */
static bool capture_motion_state(GeometryComponent &component, const Field<bool> &selection_field)
{
  return bke::try_capture_fields_on_geometry(component,
                                             {old_position_attr, old_rotation_attr},
                                             AttrDomain::Point,
                                             selection_field,
                                             {field_inputs::position(), field_inputs::rotation()});
}

/* Note: impulse is applied in object space, like the velocity attribute. */
static bool apply_impulse(GeometryComponent &component,
                          const Field<bool> &selection_field,
                          const Field<float3> &impulse)
{
  static const auto apply_impulse_fn =
      fn::multi_function::build::SI3_SO<float3, float, float3, float3>(
          "Apply Impulse",
          [](const float3 &velocity, const float inv_mass, const float3 &impulse) -> float3 {
            return velocity + inv_mass * impulse;
          });
  const GField field = Field<float3>(fn::FieldOperation::Create(
      apply_impulse_fn, {field_inputs::velocity(), field_inputs::inverse_mass(), impulse}));

  return bke::try_capture_field_on_geometry(
      component, velocity_attr, bke::AttrDomain::Point, selection_field, field);
}

/* Note: angular_impulse is expected to be in local body space, like the angular velocity
 * attribute. */
static bool apply_angular_impulse(GeometryComponent &component,
                                  const Field<bool> &selection_field,
                                  const Field<float3> &angular_impulse)
{
  static const auto apply_angular_impulse_fn =
      fn::multi_function::build::SI3_SO<float3, float3, float3, float3>(
          "Apply Angular Impulse",
          [](const float3 &angular_velocity,
             const float3 inv_inertia,
             const float3 &angular_impulse) -> float3 {
            return angular_velocity + inv_inertia * angular_impulse;
          });
  const GField field = Field<float3>(fn::FieldOperation::Create(
      apply_angular_impulse_fn,
      {field_inputs::angular_velocity(), field_inputs::inverse_inertia(), angular_impulse}));

  return bke::try_capture_field_on_geometry(
      component, angular_velocity_attr, bke::AttrDomain::Point, selection_field, field);
}

/* Note: force is applied in object space. */
static bool UNUSED_FUNCTION(apply_force)(GeometryComponent &component,
                                         const Field<bool> &selection_field,
                                         const float delta_time,
                                         const Field<float3> &force)
{
  const auto impulse_fn = fn::multi_function::build::SI1_SO<float3, float3>(
      "Compute Impulse from Force",
      [=](const float3 &force) -> float3 { return force * delta_time; });
  const GField field = Field<float3>(fn::FieldOperation::Create(impulse_fn, {force}));

  return apply_impulse(component, selection_field, field);
}

/* Note: torque is applied in local body space. */
static bool UNUSED_FUNCTION(apply_torque)(GeometryComponent &component,
                                          const Field<bool> &selection_field,
                                          const float delta_time,
                                          const Field<float3> &torque)
{
  const auto angular_impulse_fn = fn::multi_function::build::SI1_SO<float3, float3>(
      "Compute Angular Impulse from Torque",
      [=](const float3 &torque) -> float3 { return torque * delta_time; });
  const GField field = Field<float3>(fn::FieldOperation::Create(angular_impulse_fn, {torque}));

  return apply_angular_impulse(component, selection_field, field);
}

static bool integrate_velocity(GeometryComponent &component,
                               const Field<bool> &selection_field,
                               const float delta_time,
                               const float linear_factor,
                               const float3 &gravity,
                               const Field<float3> &external_force)
{
  if (linear_factor == 0.0f) {
    return true;
  }

  const auto integrate_velocity_fn =
      fn::multi_function::build::SI3_SO<float3, float, float3, float3>(
          "Integrate Velocity",
          [=](const float3 &velocity, const float inv_mass, const float3 &ext_force) -> float3 {
            return velocity + delta_time * linear_factor * (gravity + inv_mass * ext_force);
          });
  const GField field = Field<float3>(fn::FieldOperation::Create(
      integrate_velocity_fn,
      {field_inputs::velocity(), field_inputs::inverse_mass(), external_force}));

  return bke::try_capture_field_on_geometry(
      component, velocity_attr, bke::AttrDomain::Point, selection_field, field);
}

static bool integrate_angular_velocity(GeometryComponent &component,
                                       const Field<bool> &selection_field,
                                       const float delta_time,
                                       const float angular_factor,
                                       const Field<float3> &external_torque)
{
  if (angular_factor == 0.0f) {
    return true;
  }

  const auto integrate_angular_velocity_fn =
      fn::multi_function::build::SI4_SO<float3, float3, float3, float3, float3>(
          "Integrate Angular Velocity",
          [=](const float3 &angular_velocity,
              const float3 &inertia,
              const float3 &inv_inertia,
              const float3 &ext_torque) -> float3 {
            const float3 precession = math::cross(angular_velocity, angular_velocity * inertia);
            return angular_velocity +
                   delta_time * angular_factor * (inv_inertia * (ext_torque - precession));
          });
  const GField field = Field<float3>(fn::FieldOperation::Create(integrate_angular_velocity_fn,
                                                                {field_inputs::angular_velocity(),
                                                                 field_inputs::inertia(),
                                                                 field_inputs::inverse_inertia(),
                                                                 external_torque}));

  return bke::try_capture_field_on_geometry(
      component, angular_velocity_attr, bke::AttrDomain::Point, selection_field, field);
}

static bool integrate_position(GeometryComponent &component,
                               const Field<bool> &selection_field,
                               const float delta_time,
                               const float linear_factor)
{
  if (linear_factor == 0.0f) {
    return true;
  }

  const auto integrate_position_fn = fn::multi_function::build::SI2_SO<float3, float3, float3>(
      "Integrate Positions", [=](const float3 &position, const float3 &velocity) -> float3 {
        return position + linear_factor * delta_time * velocity;
      });
  const GField field = Field<float3>(fn::FieldOperation::Create(
      integrate_position_fn, {field_inputs::position(), field_inputs::velocity()}));

  return bke::try_capture_field_on_geometry(
      component, position_attr, bke::AttrDomain::Point, selection_field, field);
}

static bool integrate_rotation(GeometryComponent &component,
                               const Field<bool> &selection_field,
                               const float delta_time,
                               const float angular_factor)
{
  if (angular_factor == 0.0f) {
    return true;
  }

  const auto integrate_rotation_fn =
      fn::multi_function::build::SI2_SO<math::Quaternion, float3, math::Quaternion>(
          "Integrate Rotations",
          [=](const math::Quaternion &rotation,
              const float3 &angular_velocity) -> math::Quaternion {
            math::Quaternion direction = math::Quaternion(0, angular_velocity) * rotation;
            const float factor = angular_factor * delta_time * 0.5f;
            return math::normalize(
                math::Quaternion(rotation.w + factor * direction.w,
                                 rotation.imaginary_part() + factor * direction.imaginary_part()));
          });
  const GField field = Field<math::Quaternion>(fn::FieldOperation::Create(
      integrate_rotation_fn, {field_inputs::rotation(), field_inputs::angular_velocity()}));

  return bke::try_capture_field_on_geometry(
      component, rotation_attr, bke::AttrDomain::Point, selection_field, field);
}

static void integrate_motion(GeometryComponent &component,
                             const Field<bool> &selection_field,
                             const float delta_time,
                             const float linear_factor,
                             const float angular_factor,
                             const float3 &gravity,
                             const Field<float3> &external_force,
                             const Field<float3> &external_torque)
{
  integrate_velocity(
      component, selection_field, delta_time, linear_factor, gravity, external_force);
  integrate_angular_velocity(
      component, selection_field, delta_time, angular_factor, external_torque);

  integrate_position(component, selection_field, delta_time, linear_factor);
  integrate_rotation(component, selection_field, delta_time, angular_factor);
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
  evaluator.add(field_inputs::mass());
  evaluator.add(field_inputs::inertia());
  evaluator.add(field_inputs::old_position());
  evaluator.add(field_inputs::old_rotation());
  evaluator.add_with_destination(field_inputs::position(), variables.positions.as_mutable_span());
  evaluator.add_with_destination(field_inputs::rotation(), variables.rotations.as_mutable_span());
  evaluator.add_with_destination(field_inputs::velocity(), variables.velocities.as_mutable_span());
  evaluator.add_with_destination(field_inputs::angular_velocity(),
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
        position_attr, AttrDomain::Point);
    AttributeWriter<math::Quaternion> rotations_writer =
        attributes.lookup_or_add_for_write<math::Quaternion>(rotation_attr, AttrDomain::Point);
    AttributeWriter<float3> velocities_writer = attributes.lookup_or_add_for_write<float3>(
        velocity_attr, AttrDomain::Point);
    AttributeWriter<float3> angular_velocities_writer = attributes.lookup_or_add_for_write<float3>(
        angular_velocity_attr, AttrDomain::Point);

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
    if (!store_hair_rest_shape(hair_curves)) {
      params.error_message_add(NodeWarningType::Error, "Could not store rest shape");
    }
    init_hair_physics(
        hair_curves, selection_field, behavior, [params](NodeWarningType type, StringRef message) {
          params.error_message_add(type, message);
        });

    /* Clear any existing constraint data. */
    constraint_bundle = Bundle::create();
    generate_elastic_rod_constraints(constraint_bundle, hair_curves, selection_field, behavior);
    generate_root_attachment_constraints(
        constraint_bundle, hair_curves, selection_field, behavior);
  }

  update_constraints(constraint_bundle, behavior, params.user_data());

  IndexMaskMemory memory;
  Vector<ConstraintEvalData> constraint_data = hair_constraints::constraint_bundle_to_eval_data(
      std::move(constraint_bundle), debug_output, memory);

  /* Store current motion state for later velocity estimation. */
  capture_motion_state(hair_curves, selection_field);

  /* Unconstrained motion. */
  integrate_motion(hair_curves,
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
