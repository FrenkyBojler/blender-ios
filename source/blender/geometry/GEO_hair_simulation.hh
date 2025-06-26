/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_rotation_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"

#include "FN_field.hh"

namespace blender::geometry::hair_simulation {

namespace attributes {
const StringRef position = "position";
const StringRef rotation = "rotation";
const StringRef velocity = "velocity";
const StringRef angular_velocity = "angular_velocity";

const StringRef mass = "mass";
const StringRef inv_mass = "inv_mass";
const StringRef inertia = "inertia";
const StringRef inv_inertia = "inv_inertia";
};  // namespace attributes

namespace field_inputs {

fn::Field<float3> position();
fn::Field<math::Quaternion> rotation();
fn::Field<float3> velocity();
fn::Field<float3> angular_velocity();

fn::Field<float> mass();
fn::Field<float> inverse_mass();
fn::Field<float3> inertia();
fn::Field<float3> inverse_inertia();

fn::Field<bool> is_curve_start_point();
fn::Field<bool> is_curve_end_point();

}  // namespace field_inputs

namespace field_ops {

/**
 * Value shifted along the curve. Uses end point value for out-of-range points.
 */
fn::GField shifted_curve_value(const fn::GField &value_field, const int offset);
/**
 * Cross section area for each curve point.
 */
fn::Field<float> curve_cross_section(const fn::Field<float> radius_field);
/**
 * Second moment of inertia for each curve point.
 */
fn::Field<float3> curve_area_moment(const fn::Field<float> radius_field);
/**
 * Segment vector from a point to the next.
 */
fn::Field<float3> curve_segment(const fn::Field<float3> &position_field);
/**
 * Length of segment from each point to the next. Last segment of a curve has zero length.
 */
fn::Field<float> curve_segment_length(const fn::Field<float3> &position_field);
/**
 * Average of segments before and after each point. Start/end points only use the next/previous
 * segment respectively.
 */
fn::Field<float> staggered_curve_segment_length(const fn::Field<float3> &position_field);
/**
 * Mass of curve segments.
 */
fn::Field<float> curve_point_mass(const fn::Field<float> &segment_length_field,
                                  const fn::Field<float> &cross_section_field,
                                  const fn::Field<float> &density_field);
/**
 * Principal moment of inertia of curve segments. Z axis is the segment direction.
 */
fn::Field<float3> curve_segment_inertia(const fn::Field<float> &segment_length_field,
                                        const fn::Field<float> &radius_field,
                                        const fn::Field<float> &density_field);

/**
 * Inverse of mass.
 */
fn::Field<float> inverse_mass(const fn::Field<float> &mass_field);
/**
 * Inverse principal moments of inertia.
 */
fn::Field<float3> inverse_inertia(const fn::Field<float3> &inertia_field);

}  // namespace field_ops

/**
 * Add linear impulse to point velocities.
 * \note Impulse is applied in object space, like the velocity attribute.
 */
bool apply_impulse(bke::GeometryComponent &component,
                   const fn::Field<bool> &selection_field,
                   const fn::Field<float3> &impulse);
/**
 * Apply angular impulse to point angular velocities.
 * \note Angular_impulse is applied in local body space, like the angular velocity attribute.
 */
bool apply_angular_impulse(bke::GeometryComponent &component,
                           const fn::Field<bool> &selection_field,
                           const fn::Field<float3> &angular_impulse);
/**
 * Apply a linear force to points.
 * \note Force vector is applied in object space.
 */
bool apply_force(bke::GeometryComponent &component,
                 const fn::Field<bool> &selection_field,
                 const float delta_time,
                 const fn::Field<float3> &force);
/**
 * Apply torque to points.
 * \note Torque vector is applied in local body space.
 */
bool apply_torque(bke::GeometryComponent &component,
                  const fn::Field<bool> &selection_field,
                  const float delta_time,
                  const fn::Field<float3> &torque);

/**
 * Integrate the motion of points over a time step.
 *
 * \param delta_time The duration of the time step.
 * \param linear_factor Influence of linear forces and velocities.
 * \param angular_factor Influence of torque and angular velocities.
 * \param gravity Constant uniform acceleration.
 * \param external_force Linear force applied to points in object space.
 * \param external_torque Torque applied to points in local body space.
 */
void integrate_motion(bke::GeometryComponent &component,
                      const fn::Field<bool> &selection_field,
                      const float delta_time,
                      const float linear_factor,
                      const float angular_factor,
                      const float3 &gravity,
                      const fn::Field<float3> &external_force,
                      const fn::Field<float3> &external_torque);

}  // namespace blender::geometry::hair_simulation
