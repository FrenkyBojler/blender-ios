/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"

#include "GEO_hair_simulation.hh"

#include "FN_multi_function_builder.hh"

namespace blender::geometry::hair_simulation {

using bke::AttrDomain;
using bke::AttributeFieldInput;
using bke::GeometryComponent;
using fn::Field;
using fn::GField;

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

namespace field_inputs {

Field<float3> position()
{
  return AttributeFieldInput::Create<float3>(attributes::position);
}

Field<math::Quaternion> rotation()
{
  return AttributeFieldInput::Create<math::Quaternion>(attributes::rotation);
}

Field<float3> velocity()
{
  return AttributeFieldInput::Create<float3>(attributes::velocity);
}

Field<float3> angular_velocity()
{
  return AttributeFieldInput::Create<float3>(attributes::angular_velocity);
}

Field<float> mass()
{
  return AttributeFieldInput::Create<float>(attributes::mass);
}

Field<float> inverse_mass()
{
  return AttributeFieldInput::Create<float>(attributes::inv_mass);
}

Field<float3> inertia()
{
  return AttributeFieldInput::Create<float3>(attributes::inertia);
}

Field<float3> inverse_inertia()
{
  return AttributeFieldInput::Create<float3>(attributes::inv_inertia);
}

Field<bool> is_curve_start_point()
{
  return Field<bool>{std::make_shared<IsStartPointFieldInput>()};
}

Field<bool> is_curve_end_point()
{
  return Field<bool>{std::make_shared<IsEndPointFieldInput>()};
}

}  // namespace field_inputs

namespace field_ops {

GField shifted_curve_value(const GField &value_field, const int offset)
{
  Field<int> index_field{std::make_shared<ShiftedIndexOnCurveInput>(offset)};
  return GField{std::make_shared<bke::EvaluateAtIndexInput>(
      std::move(index_field), value_field, AttrDomain::Point)};
}

Field<float> curve_cross_section(const Field<float> radius_field)
{
  static const auto cross_section_fn = fn::multi_function::build::SI1_SO<float, float>(
      "Rod Cross Section", [](const float radius) -> float { return M_PI * radius * radius; });
  return Field<float>(fn::FieldOperation::Create(cross_section_fn, {radius_field}));
}

Field<float3> curve_area_moment(const Field<float> radius_field)
{
  static const auto area_moment_fn = fn::multi_function::build::SI1_SO<float, float3>(
      "Second Moment of Area", [](const float radius) -> float3 {
        const float radius_sq = radius * radius;
        return radius_sq * radius_sq * M_PI * float3(0.25f, 0.25f, 0.5f);
      });
  return Field<float3>(fn::FieldOperation::Create(area_moment_fn, {radius_field}));
}

Field<float> curve_segment_length(const Field<float3> &position_field)
{
  static const auto segment_length_fn = fn::multi_function::build::SI2_SO<float3, float3, float>(
      "Segment Length", [](const float3 &pt, const float3 &pt_next) -> float {
        return math::distance(pt, pt_next);
      });
  return Field<float>(fn::FieldOperation::Create(
      segment_length_fn, {position_field, field_ops::shifted_curve_value(position_field, 1)}));
}

Field<float> staggered_curve_segment_length(const Field<float3> &position_field)
{
  static const auto avg_segment_length_fn =
      fn::multi_function::build::SI4_SO<float, float, bool, bool, float>(
          "Staggered Segment Length",
          [](const float length_prev, const float length, const bool is_start, const bool is_end)
              -> float {
            const float weight_prev = !is_start;
            const float weight = !is_end;
            return math::safe_divide(length_prev * weight_prev + length * weight,
                                     weight_prev + weight);
          });
  Field<float> segment_length_field = curve_segment_length(position_field);
  return Field<float>(
      fn::FieldOperation::Create(avg_segment_length_fn,
                                 {field_ops::shifted_curve_value(segment_length_field, -1),
                                  segment_length_field,
                                  field_inputs::is_curve_start_point(),
                                  field_inputs::is_curve_end_point()}));
}

Field<float> curve_point_mass(const Field<float> &segment_length_field,
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

Field<float3> curve_segment_inertia(const Field<float> &segment_length_field,
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

Field<float> inverse_mass(const Field<float> &mass_field)
{
  static const auto inv_mass_fn = fn::multi_function::build::SI1_SO<float, float>(
      "Inverse Mass", [](const float mass) -> float { return math::safe_rcp(mass); });
  return Field<float>(fn::FieldOperation::Create(inv_mass_fn, {mass_field}));
}

Field<float3> inverse_inertia(const Field<float3> &inertia_field)
{
  static const auto inv_inertia_fn = fn::multi_function::build::SI1_SO<float3, float3>(
      "Inverse Moment of Inertia",
      [](const float3 &inertia) -> float3 { return math::safe_rcp(inertia); });
  return Field<float3>(fn::FieldOperation::Create(inv_inertia_fn, {inertia_field}));
}

}  // namespace field_ops

/* Note: impulse is applied in object space, like the velocity attribute. */
bool apply_impulse(GeometryComponent &component,
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
      component, attributes::velocity, AttrDomain::Point, selection_field, field);
}

/* Note: angular_impulse is expected to be in local body space, like the angular velocity
 * attribute. */
bool apply_angular_impulse(GeometryComponent &component,
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
      component, attributes::angular_velocity, AttrDomain::Point, selection_field, field);
}

/* Note: force is applied in object space. */
bool apply_force(GeometryComponent &component,
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
bool apply_torque(GeometryComponent &component,
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
      component, attributes::velocity, AttrDomain::Point, selection_field, field);
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
      component, attributes::angular_velocity, AttrDomain::Point, selection_field, field);
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
      component, attributes::position, AttrDomain::Point, selection_field, field);
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
      component, attributes::rotation, AttrDomain::Point, selection_field, field);
}

void integrate_motion(GeometryComponent &component,
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

}  // namespace blender::geometry::hair_simulation
