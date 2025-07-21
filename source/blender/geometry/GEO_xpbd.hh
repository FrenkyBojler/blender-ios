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

class LocalXpbdConstraintCorrections {
 public:
  Vector<PositionCorrection> position_corrections_;

  void add_position_correction(const int geometry_i,
                               const int position_i,
                               const float3 &correction)
  {
    position_corrections_.append({geometry_i, position_i, correction});
  }
};

class SimForce {
 public:
  fn::Field<float3> force_field;
};

class SimAcceleration {
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

  SimGeometry(const SimGeometrySet &src, GeometryVariant data)
      : data(data),
        path(src.path),
        mass_attribute(src.mass_attribute),
        velocity_attribute(src.velocity_attribute)
  {
  }

  std::optional<bke::AttributeAccessor> attributes() const;
  std::optional<bke::MutableAttributeAccessor> attributes_for_write();

  int set_point_field_context(std::optional<bke::GeometryFieldContext> &r_context) const;
};

class XpbdConstraintCorrections {
 private:
  threading::EnumerableThreadSpecific<LocalXpbdConstraintCorrections> local_corrections_;

 public:
  LocalXpbdConstraintCorrections &local()
  {
    return local_corrections_.local();
  }

  void apply(MutableSpan<SimGeometry> sim_geometries);
};

class XpbdContraints {
 public:
  virtual void ensure_init(MutableSpan<SimGeometry> /*sim_geometries*/) {}
  virtual void solve(const Span<SimGeometry> /*sim_geometries*/,
                     XpbdConstraintCorrections & /*corrections*/)
  {
  }
  virtual void post_solve_apply(MutableSpan<SimGeometry> /*sim_geometries*/) {}
};

XpbdContraints &create_constraint__edge_lengths(ResourceScope &scope,
                                                std::string rest_length_attribute);

XpbdContraints &create_constraint__fixed_positions(ResourceScope &scope,
                                                   fn::Field<bool> selection_field,
                                                   fn::Field<float3> fixed_positions_field);

}  // namespace blender::geometry::xpbd
