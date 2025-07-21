/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_mutex.hh"
#include "BLI_vector.hh"

#include "FN_field.hh"

namespace blender::geometry::xpbd {

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
  float quantize_scale = 10000.0f;

  SimGeometry(const SimGeometrySet &src, GeometryVariant data);

  std::optional<bke::AttributeAccessor> attributes() const;
  std::optional<bke::MutableAttributeAccessor> attributes_for_write();
  int points_num() const;

  int set_point_field_context(std::optional<bke::GeometryFieldContext> &r_context) const;
};

struct PositionCorrection {
  Mutex mutex;
  int3 offset = {0, 0, 0};
  int num_corrections = 0;
};

class ConstraintCorrections;

class LocalConstraintCorrections {
  ConstraintCorrections &corrections_;

 public:
  LocalConstraintCorrections(ConstraintCorrections &corrections);
  void add_position_correction(int geometry_i, int position_i, const float3 &offset);
};

class ConstraintCorrections {
 private:
  MutableSpan<SimGeometry> sim_geometries_;
  Array<Array<PositionCorrection>> corrections_;
  LocalConstraintCorrections local_corrections_;

  friend LocalConstraintCorrections;

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

inline void LocalConstraintCorrections::add_position_correction(const int geometry_i,
                                                                const int position_i,
                                                                const float3 &offset)
{
  const float quantize_scale = corrections_.sim_geometries_[geometry_i].quantize_scale;
  const int3 quantized_offset = int3(offset * quantize_scale);
  PositionCorrection &correction = corrections_.corrections_[geometry_i][position_i];
  std::lock_guard lock(correction.mutex);
  correction.offset += quantized_offset;
  correction.num_corrections++;
}

}  // namespace blender::geometry::xpbd
