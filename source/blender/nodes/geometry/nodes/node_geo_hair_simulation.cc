/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "BKE_anonymous_attribute_make.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_type_conversions.hh"

#include "FN_field.hh"

#include "GEO_hair_solver.hh"

#include "NOD_geo_hair_constraints.hh"
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
static const std::string velocity_attr = "velocity";
static const std::string angular_velocity_attr = "angular_velocity";
static const std::string mass_attr = "mass";
static const std::string inv_mass_attr = "inv_mass";
static const std::string inertia_attr = "inertia";
static const std::string inv_inertia_attr = "inv_inertia";
static const std::string radius_attr = "radius";
static const std::string segment_length_attr = "segment_length";

/* XXX These should be anonymous attributes. */
static const std::string position_cache_attr = ".old_position";
static const std::string rotation_cache_attr = ".old_rotation";
static const std::string cross_section_attr = ".cross_section";
static const std::string area_moment_attr = ".area_moment";

namespace fields {

using namespace bke;

/* Shift point indices along the curve.
 * Indices at the start or end are clamped to the curve range. */
class ShiftedIndexOnCurveInput final : public CurvesFieldInput {
 private:
  int offset_;

 public:
  ShiftedIndexOnCurveInput(const int offset)
      : CurvesFieldInput(CPPType::get<int>(), "Shifted Index on Curve"), offset_(offset)
  {
  }

  GVArray get_varray_for_context(const CurvesGeometry &curves,
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

  std::optional<AttrDomain> preferred_domain(const CurvesGeometry & /*curves*/) const final
  {
    return AttrDomain::Point;
  }
};

static GField create_shifted_curve_input(const GField &value_field, const int offset)
{
  Field<int> index_field{std::make_shared<ShiftedIndexOnCurveInput>(offset)};
  return GField{std::make_shared<EvaluateAtIndexInput>(
      std::move(index_field), value_field, AttrDomain::Point)};
}

static Field<float> create_cross_section(const Field<float> radius_field)
{
  static const auto cross_section_fn = fn::multi_function::build::SI1_SO<float, float>(
      "Rod Cross Section", [](const float radius) -> float { return M_PI * radius * radius; });
  return Field<float>(fn::FieldOperation::Create(cross_section_fn, {radius_field}));
}

static Field<float3> create_area_moment(const Field<float> radius_field)
{
  static const auto area_moment_fn = fn::multi_function::build::SI1_SO<float, float3>(
      "Second Moment of Area", [](const float radius) -> float3 {
        const float radius_sq = radius * radius;
        return radius_sq * radius_sq * M_PI * float3(0.25f, 0.25f, 0.5f);
      });
  return Field<float3>(fn::FieldOperation::Create(area_moment_fn, {radius_field}));
}

static Field<float> create_segment_length(const Field<float3> &position_field)
{
  static const auto segment_length_fn = fn::multi_function::build::SI2_SO<float3, float3, float>(
      "Segment Length",
      [](const float3 &p1, const float3 &p2) -> float { return math::distance(p1, p2); });
  Field<float3> next_position_field = fields::create_shifted_curve_input(position_field, 1);
  return Field<float>(fn::FieldOperation::Create(
      segment_length_fn, {position_field, std::move(next_position_field)}));
}

/* Average segment length from both sides of a point to determine average volume. */
static Field<float> create_average_segment_length(const Field<float> &segment_length_field)
{
  static const auto avg_segment_length_fn = fn::multi_function::build::SI2_SO<float, float, float>(
      "Mean Segment Length", [](const float length1, const float length2) -> float {
        return 0.5f * (length1 + length2);
      });
  Field<float> prev_segment_length_field = fields::create_shifted_curve_input(segment_length_field,
                                                                              -1);
  return Field<float>(fn::FieldOperation::Create(
      avg_segment_length_fn, {segment_length_field, std::move(prev_segment_length_field)}));
}

static Field<float> create_point_mass_field(const Field<float> &segment_length_field,
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

static Field<float3> create_segment_inertia_field(const Field<float> &segment_length_field,
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

static Field<float> create_inverse_mass_field(const Field<float> &mass_field)
{
  static const auto inv_mass_fn = fn::multi_function::build::SI1_SO<float, float>(
      "Inverse Mass", [](const float mass) -> float { return math::safe_rcp(mass); });
  return Field<float>(fn::FieldOperation::Create(inv_mass_fn, {mass_field}));
}

static Field<float3> create_inverse_inertia_field(const Field<float3> &inertia_field)
{
  static const auto inv_inertia_fn = fn::multi_function::build::SI1_SO<float3, float3>(
      "Inverse Moment of Inertia",
      [](const float3 &inertia) -> float3 { return math::safe_rcp(inertia); });
  return Field<float3>(fn::FieldOperation::Create(inv_inertia_fn, {inertia_field}));
}

}  // namespace fields

enum class VectorSpace {
  /* Object space. */
  Object,
  /* Local space of an elemental rigid body, aligned with principal axes. */
  BodyLocal,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Delta Time").min(0.0f).hide_value();
  b.add_input<decl::Int>("Constraint Iterations").default_value(5).min(0);
  b.add_input<decl::Geometry>("Hair").supported_type(bke::GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Float>("Density").default_value(1000.0f).field_on_all();
  b.add_input<decl::Vector>("Gravity").default_value(float3(0, 0, -9.81f)).hide_value();
  b.add_input<decl::Vector>("Force").field_on_all().hide_value();
  b.add_input<decl::Vector>("Torque").field_on_all().hide_value();

  b.add_output<decl::Geometry>("Hair").propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  UNUSED_VARS(layout, ptr);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  UNUSED_VARS(node);
}

static bool store_hair_rest_shape(GeometryComponent &hair_component)
{
  Field<float3> position_field{AttributeFieldInput::Create<float3>("position")};
  Field<float3> normal_field{std::make_shared<bke::NormalFieldInput>(false, false)};
  return bke::try_capture_fields_on_geometry(hair_component,
                                             {"rest_position", "rest_normal"},
                                             AttrDomain::Point,
                                             {position_field, normal_field});
}

/* Capture hair attributes for mass, moments of inertia, rod stiffness and damping. */
static bool init_hair_physics(GeometryComponent &component,
                              const Field<bool> &selection_field,
                              const Field<float> &density_field)
{
  const Field<float> radius_field = bke::AttributeFieldInput::Create<float>(radius_attr);
  const Field<float> cross_section_field = fields::create_cross_section(radius_field);
  const Field<float3> area_moment_field = fields::create_area_moment(radius_field);
  if (!bke::try_capture_fields_on_geometry(component,
                                           {cross_section_attr, area_moment_attr},
                                           bke::AttrDomain::Point,
                                           selection_field,
                                           {cross_section_field, area_moment_field}))
  {
    return false;
  }

  const Field<float3> position_field{bke::AttributeFieldInput::Create<float3>(position_attr)};
  const Field<float> segment_length_field = fields::create_segment_length(position_field);
  if (!bke::try_capture_field_on_geometry(component,
                                          segment_length_attr,
                                          bke::AttrDomain::Point,
                                          selection_field,
                                          segment_length_field))
  {
    return false;
  }

  const Field<float> avg_segment_length_field = fields::create_average_segment_length(
      segment_length_field);
  const Field<float> point_mass_field = fields::create_point_mass_field(
      avg_segment_length_field,
      AttributeFieldInput::Create<float>(cross_section_attr),
      density_field);
  const Field<float3> segment_inertia_field = fields::create_segment_inertia_field(
      avg_segment_length_field,
      AttributeFieldInput::Create<float3>(area_moment_attr),
      density_field);

  const Field<float> inv_point_mass_field = fields::create_inverse_mass_field(point_mass_field);
  const Field<float3> inv_segment_inertia_field = fields::create_inverse_inertia_field(
      segment_inertia_field);

  if (!bke::try_capture_fields_on_geometry(
          component,
          {mass_attr, inertia_attr, inv_mass_attr, inv_inertia_attr},
          bke::AttrDomain::Point,
          selection_field,
          {point_mass_field,
           segment_inertia_field,
           inv_point_mass_field,
           inv_segment_inertia_field}))
  {
    return false;
  }

  return true;
}

/* Create internal stretch/shear and bending constraints. */
static void generate_elastic_rod_constraints(BundlePtr &bundle,
                                             const GeometryComponent &hair_component,
                                             const Field<bool> selection_field)
{
  GeometrySet stretch_constraints;
  GeometrySet bending_constraints;

  UNUSED_VARS(hair_component, selection_field);

  hair_constraints::set_constraints(bundle, ConstraintType::StretchShear, stretch_constraints);
  hair_constraints::set_constraints(bundle, ConstraintType::BendTwist, bending_constraints);
}

/* Create root attachment constraints. */
static void generate_root_attachment_constraints(BundlePtr &bundle,
                                                 const GeometryComponent &hair_component,
                                                 const Field<bool> selection_field)
{
  GeometrySet position_constraints;
  GeometrySet rotation_constraints;

  UNUSED_VARS(hair_component, selection_field);

  hair_constraints::set_constraints(bundle, ConstraintType::PositionGoal, position_constraints);
  hair_constraints::set_constraints(bundle, ConstraintType::RotationGoal, rotation_constraints);
}

/* Capture motions state attribute for later velocity estimation. */
static bool capture_motion_state(GeometryComponent &component, const Field<bool> &selection_field)
{
  const Field<float3> position_field{AttributeFieldInput::Create<float3>(position_attr)};
  const Field<math::Quaternion> rotation_field{
      AttributeFieldInput::Create<math::Quaternion>(rotation_attr)};
  return bke::try_capture_fields_on_geometry(
      component,
      {position_cache_attr, rotation_cache_attr},
      AttrDomain::Point,
      selection_field,
      {std::move(position_field), std::move(rotation_field)});
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
  const GField field = Field<float3>(
      fn::FieldOperation::Create(apply_impulse_fn,
                                 {bke::AttributeFieldInput::Create<float3>(velocity_attr),
                                  bke::AttributeFieldInput::Create<float>(inv_mass_attr),
                                  impulse}));

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
  const GField field = Field<float3>(
      fn::FieldOperation::Create(apply_angular_impulse_fn,
                                 {bke::AttributeFieldInput::Create<float3>(angular_velocity_attr),
                                  bke::AttributeFieldInput::Create<float3>(inv_inertia_attr),
                                  bke::AttributeFieldInput::Create<float3>(rotation_attr),
                                  angular_impulse}));

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
  const GField field = Field<float3>(
      fn::FieldOperation::Create(integrate_velocity_fn,
                                 {bke::AttributeFieldInput::Create<float3>(velocity_attr),
                                  bke::AttributeFieldInput::Create<float>(inv_mass_attr),
                                  external_force}));

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
  const GField field = Field<float3>(
      fn::FieldOperation::Create(integrate_angular_velocity_fn,
                                 {bke::AttributeFieldInput::Create<float3>(angular_velocity_attr),
                                  bke::AttributeFieldInput::Create<float3>(inertia_attr),
                                  bke::AttributeFieldInput::Create<float3>(inv_inertia_attr),
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
  const GField field = Field<float3>(
      fn::FieldOperation::Create(integrate_position_fn,
                                 {bke::AttributeFieldInput::Create<float3>(position_attr),
                                  bke::AttributeFieldInput::Create<float3>(velocity_attr)}));

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
      integrate_rotation_fn,
      {bke::AttributeFieldInput::Create<math::Quaternion>(rotation_attr),
       bke::AttributeFieldInput::Create<float3>(angular_velocity_attr)}));

  return bke::try_capture_field_on_geometry(
      component, rotation_attr, bke::AttrDomain::Point, selection_field, field);
}

static void cosserat_rod_dynamics_integration(GeometryComponent &component,
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

static void UNUSED_FUNCTION(zero_init_solver)(MutableSpan<ConstraintEvalData> constraint_data)
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

static void UNUSED_FUNCTION(warm_start_solver)(const ConstraintEvalParams &eval_params,
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

static void UNUSED_FUNCTION(estimate_velocity)(ConstraintEvalParams &params,
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

static void UNUSED_FUNCTION(do_position_constraints_iteration)(
    const ConstraintEvalParams &eval_params,
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

static void UNUSED_FUNCTION(do_velocity_constraints_iteration)(
    const ConstraintEvalParams &eval_params,
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

static void UNUSED_FUNCTION(solve_constraints)(GeometrySet &hair_geometry, const int iterations)
{
  /* TODO warm start doesn't work properly yet. */
  const bool warm_start = false;
  UNUSED_VARS(hair_geometry, iterations, warm_start);

  // Field<float> mass_field = params.extract_input<Field<float>>("Mass");
  // Field<float3> inertia_field = params.extract_input<Field<float3>>("Inertia");
  // Field<float3> old_position_field = params.extract_input<Field<float3>>("Old Position");
  // Field<math::Quaternion> old_rotation_field = params.extract_input<Field<math::Quaternion>>(
  //     "Old Rotation");
  // Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  // Field<math::Quaternion> rotation_field = params.extract_input<Field<math::Quaternion>>(
  //     "Rotation");
  // Field<float3> velocity_field = params.extract_input<Field<float3>>("Velocity");
  // Field<float3> angular_velocity_field = params.extract_input<Field<float3>>("Angular
  // Velocity"); std::optional<std::string> position_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Position");
  // std::optional<std::string> rotation_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Rotation");
  // std::optional<std::string> velocity_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Velocity");
  // std::optional<std::string> angular_velocity_output_id =
  //     params.get_output_anonymous_attribute_id_if_needed("Angular Velocity");

  // GeometrySet colliders_geometry_set = params.extract_input<GeometrySet>("Colliders");
  // Span<float4x4> collider_transforms = colliders_geometry_set.has_instances() ?
  //                                          colliders_geometry_set.get_instances()->transforms()
  //                                          : Span<float4x4>{};

  // ConstraintEvalParams eval_params = extract_eval_params(params);
  // const bool debug_output = (eval_params.debug_recorder != nullptr);

  // Vector<ConstraintEvalData> constraint_data;
  // IndexMaskMemory memory;
  // get_constraint_data(params, debug_output, constraint_data, memory);

  // init_constraints(init_mode, eval_params, constraint_data);

  // static const Array<GeometryComponent::Type> types = {bke::GeometryComponent::Type::Mesh,
  //                                                      bke::GeometryComponent::Type::PointCloud,
  //                                                      bke::GeometryComponent::Type::Curve,
  //                                                      bke::GeometryComponent::Type::GreasePencil};
  // geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
  //   for (const bke::GeometryComponent::Type component_type : types) {
  //     if (geometry_set.has(component_type)) {
  //       bke::GeometryComponent &component =
  //       geometry_set.get_component_for_write(component_type);
  //       std::optional<bke::MutableAttributeAccessor> attributes =
  //       component.attributes_for_write(); if (!attributes) {
  //         continue;
  //       }

  //      if (eval_params.debug_recorder) {
  //        eval_params.debug_recorder->set_geometry(geometry_set, component_type);
  //      }

  //      const int num_points = attributes->domain_size(AttrDomain::Point);
  //      ConstraintVariables vars;
  //      vars.positions.reinitialize(num_points);
  //      vars.rotations.reinitialize(num_points);
  //      vars.velocities.reinitialize(num_points);
  //      vars.angular_velocities.reinitialize(num_points);

  //      const bke::GeometryFieldContext field_context{component, AttrDomain::Point};
  //      fn::FieldEvaluator evaluator{field_context, num_points};
  //      evaluator.add(mass_field);
  //      evaluator.add(inertia_field);
  //      evaluator.add(old_position_field);
  //      evaluator.add(old_rotation_field);
  //      evaluator.add_with_destination(position_field, vars.positions.as_mutable_span());
  //      evaluator.add_with_destination(rotation_field, vars.rotations.as_mutable_span());
  //      evaluator.add_with_destination(velocity_field, vars.velocities.as_mutable_span());
  //      evaluator.add_with_destination(angular_velocity_field,
  //                                     vars.angular_velocities.as_mutable_span());
  //      evaluator.evaluate();
  //      eval_params.masses = evaluator.get_evaluated<float>(0);
  //      eval_params.local_inertia = evaluator.get_evaluated<float3>(1);
  //      eval_params.old_positions = evaluator.get_evaluated<float3>(2);
  //      eval_params.old_rotations = evaluator.get_evaluated<math::Quaternion>(3);

  //      eval_params.collider_transforms = collider_transforms;
  //      /* XXX Transforms of the previous frame are not currently available, these are always the
  //       * same as the current frame. Eventually this will allow transfer of velocity from
  //       animated
  //       * colliders. */
  //      eval_params.old_collider_transforms = eval_params.collider_transforms;

  //      // execute_solver_method_on_geometry(solver_method,
  //      //                                   eval_params,
  //      //                                   constraint_data,
  //      //                                   vars,
  //      //                                   gauss_seidel_iterations,
  //      //                                   jacobi_iterations);
  //      if (eval_params.debug_recorder) {
  //        const std::string label = fmt::format("Initialize Gauss-Seidel, ");
  //        eval_params.debug_recorder->record_step(label, nullptr, -1, {}, variables);
  //      }

  //      for ([[maybe_unused]] const int i : IndexRange(gauss_seidel_iterations)) {
  //        do_gauss_seidel_iteration(
  //            EvaluationTarget::Positions, eval_params, constraint_data, variables);
  //      }

  //      estimate_velocity(eval_params, orig_velocities, orig_angular_velocities, variables);

  //      do_gauss_seidel_iteration(
  //          EvaluationTarget::Velocities, eval_params, constraint_data, variables);

  //      if (position_output_id) {
  //        AttributeWriter<float3> positions_writer = attributes->lookup_or_add_for_write<float3>(
  //            *position_output_id, AttrDomain::Point);
  //        BLI_assert(vars.positions.size() == num_points);
  //        positions_writer.varray.set_all(vars.positions);
  //        positions_writer.finish();
  //      }
  //      if (rotation_output_id) {
  //        AttributeWriter<math::Quaternion> rotations_writer =
  //            attributes->lookup_or_add_for_write<math::Quaternion>(*rotation_output_id,
  //                                                                  AttrDomain::Point);
  //        BLI_assert(vars.rotations.size() == num_points);
  //        rotations_writer.varray.set_all(vars.rotations);
  //        rotations_writer.finish();
  //      }
  //      if (velocity_output_id) {
  //        AttributeWriter<float3> velocities_writer =
  //        attributes->lookup_or_add_for_write<float3>(
  //            *velocity_output_id, AttrDomain::Point);
  //        BLI_assert(vars.velocities.size() == num_points);
  //        velocities_writer.varray.set_all(vars.velocities);
  //        velocities_writer.finish();
  //      }
  //      if (angular_velocity_output_id) {
  //        AttributeWriter<float3> angular_velocities_writer =
  //            attributes->lookup_or_add_for_write<float3>(*angular_velocity_output_id,
  //                                                        AttrDomain::Point);
  //        BLI_assert(vars.angular_velocities.size() == num_points);
  //        angular_velocities_writer.varray.set_all(vars.angular_velocities);
  //        angular_velocities_writer.finish();
  //      }
  //    }
  //  }
  //});

  // params.set_output("Geometry", geometry_set);
  // set_constraint_data_output(params, constraint_data);
  // if (eval_params.debug_recorder) {
  //   params.set_output("Debug Steps", eval_params.debug_recorder->debug_steps());
  // }
}

// static ConstraintEvalParams extract_eval_params(GeoNodeExecParams params)
//{
//   const float delta_time = std::max(params.extract_input<float>("Delta Time"), 0.0f);
//   const float delta_time_squared = delta_time * delta_time;
//   const float inv_delta_time = math::safe_rcp(delta_time);
//   const float inv_delta_time_squared = math::safe_rcp(delta_time_squared);
//   const bool debug_check = params.extract_input<bool>("Debug Checks");
//   const bool use_debug_steps = params.output_is_required("Debug Steps");
//
//   ConstraintEvalParams eval_params;
//   eval_params.delta_time = delta_time;
//   eval_params.delta_time_squared = delta_time_squared;
//   eval_params.inv_delta_time = inv_delta_time;
//   eval_params.inv_delta_time_squared = inv_delta_time_squared;
//   eval_params.error_message_add = [params](const StringRef message) {
//     params.error_message_add(NodeWarningType::Warning, message);
//   };
//   eval_params.debug_check = debug_check;
//   if (use_debug_steps) {
//     eval_params.debug_recorder = std::make_unique<xpbd_constraints::DebugRecorder>(
//         params.extract_input<GeometrySet>("Debug Steps"));
//   }
//
//   return eval_params;
// }

static void node_geo_exec(GeoNodeExecParams params)
{
  const float delta_time = std::max(params.extract_input<float>("Delta Time"), 0.0f);
  // const int constraint_iterations = std::max(params.extract_input<int>("Constraint Iterations"),
  //                                            0);
  GeometrySet hair_geometry = params.extract_input<GeometrySet>("Hair");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<float> density_field = params.extract_input<Field<float>>("Density");
  float3 gravity = params.extract_input<float3>("Gravity");
  Field<float3> force_field = params.extract_input<Field<float3>>("Force");
  Field<float3> torque_field = params.extract_input<Field<float3>>("Torque");

  if (!hair_geometry.has_curves()) {
    params.set_default_remaining_outputs();
    return;
  }
  CurveComponent &hair_curves = hair_geometry.get_component_for_write<CurveComponent>();

  /* Zero time step initializes the hair simulation. */
  if (delta_time == 0.0f) {
    if (!store_hair_rest_shape(hair_curves)) {
      params.error_message_add(NodeWarningType::Error, "Could not store rest shape");
    }
    init_hair_physics(hair_curves, selection_field, density_field);

    BundlePtr constraint_bundle = Bundle::create();
    generate_elastic_rod_constraints(constraint_bundle, hair_curves, selection_field);
    generate_root_attachment_constraints(constraint_bundle, hair_curves, selection_field);
  }

  /* Store current motion state for later velocity estimation. */
  capture_motion_state(hair_curves, selection_field);

  /* Unconstrained motion. */

  cosserat_rod_dynamics_integration(
      hair_curves, selection_field, delta_time, 1.0f, 1.0f, gravity, force_field, torque_field);

  /* Remove temporary captured attributes. */
  hair_curves.attributes_for_write()->remove(position_cache_attr);
  hair_curves.attributes_for_write()->remove(rotation_cache_attr);
  hair_curves.attributes_for_write()->remove(cross_section_attr);
  hair_curves.attributes_for_write()->remove(area_moment_attr);

  params.set_output("Hair", std::move(hair_geometry));
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
