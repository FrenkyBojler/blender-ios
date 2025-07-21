/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "FN_field.hh"

namespace blender::geometry::xpbd {

struct PositionCorrection {
  int geometry_i;
  int position_i;
  float3 correction;
};

class LocalConstraintCorrections {
 public:
  Vector<PositionCorrection> position_corrections_;

  void add_position_correction(const int geometry_i, const int position_i, const float3 &gradient)
  {
    position_corrections_.append({geometry_i, position_i, gradient});
  }
};

class ForceField {
 public:
  fn::Field<float3> force_field;
};

class AccelerationField {
 public:
  fn::Field<float3> acceleration_field;
};

struct SimGeometrySet {
  std::string path;
  bke::GeometrySet geometry;
  std::string mass_attribute;
  std::string velocity_attribute;
};

struct SimGeometry {
  using GeometryVariant = std::variant<Mesh *, PointCloud *, Curves *>;
  GeometryVariant data;
  std::string path;
  std::string mass_attribute;
  std::string velocity_attribute;

  SimGeometry(const SimGeometrySet &src, GeometryVariant data);

  std::optional<bke::AttributeAccessor> attributes() const;
  std::optional<bke::MutableAttributeAccessor> attributes_for_write();

  int set_point_field_context(std::optional<bke::GeometryFieldContext> &r_context) const;
};

class ConstraintCorrections {
 private:
  MutableSpan<SimGeometry> sim_geometries_;
  threading::EnumerableThreadSpecific<LocalConstraintCorrections> local_corrections_;

 public:
  ConstraintCorrections(MutableSpan<SimGeometry> sim_geometries);
  LocalConstraintCorrections &local();
  void apply();
};

class ConstraintSet {
 public:
  virtual void ensure_init(MutableSpan<SimGeometry> sim_geometries);
  virtual void solve(const Span<SimGeometry> sim_geometries, ConstraintCorrections &corrections);
  virtual void post_solve_apply(MutableSpan<SimGeometry> sim_geometries);
};

struct Behaviors {
  Vector<SimGeometrySet> sim_geometry_sets;
  Vector<ForceField> force_fields;
  Vector<AccelerationField> acceleration_fields;
  Vector<ConstraintSet *> constraint_sets;
};

void solve(Behaviors &behaviors, float delta_time, int substeps);

ConstraintSet &create_constraint__edge_lengths(ResourceScope &scope,
                                               std::string rest_length_attribute);

ConstraintSet &create_constraint__fixed_positions(ResourceScope &scope,
                                                  fn::Field<bool> selection_field,
                                                  fn::Field<float3> fixed_positions_field);

}  // namespace blender::geometry::xpbd
