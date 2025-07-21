/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"
#include "GEO_xpbd.hh"

namespace blender::geometry::xpbd {

constexpr StringRefNull prev_position_name = ".prev_position";

SimGeometry::SimGeometry(const SimGeometrySet &src, GeometryVariant data)
    : data(data),
      path(src.path),
      mass_attribute(src.mass_attribute),
      velocity_attribute(src.velocity_attribute)
{
}

std::optional<bke::AttributeAccessor> SimGeometry::attributes() const
{
  if (const Mesh *const *mesh = std::get_if<Mesh *>(&data)) {
    return (*mesh)->attributes();
  }
  if (const PointCloud *const *pointcloud = std::get_if<PointCloud *>(&data)) {
    return (*pointcloud)->attributes();
  }
  if (const Curves *const *curves = std::get_if<Curves *>(&data)) {
    return (*curves)->geometry.wrap().attributes();
  }
  return std::nullopt;
}

std::optional<bke::MutableAttributeAccessor> SimGeometry::attributes_for_write()
{
  if (Mesh **mesh = std::get_if<Mesh *>(&data)) {
    return (*mesh)->attributes_for_write();
  }
  if (PointCloud **pointcloud = std::get_if<PointCloud *>(&data)) {
    return (*pointcloud)->attributes_for_write();
  }
  if (Curves **curves = std::get_if<Curves *>(&data)) {
    return (*curves)->geometry.wrap().attributes_for_write();
  }
  return std::nullopt;
}

int SimGeometry::points_num() const
{
  if (std::optional<bke::AttributeAccessor> attributes = this->attributes()) {
    return attributes->domain_size(bke::AttrDomain::Point);
  }
  return 0;
}

int SimGeometry::set_point_field_context(std::optional<bke::GeometryFieldContext> &r_context) const
{
  if (const Mesh *const *mesh = std::get_if<Mesh *>(&data)) {
    r_context.emplace(**mesh, bke::AttrDomain::Point);
    return (*mesh)->verts_num;
  }
  if (const PointCloud *const *pointcloud = std::get_if<PointCloud *>(&data)) {
    r_context.emplace(**pointcloud, bke::AttrDomain::Point);
    return (*pointcloud)->totpoint;
  }
  if (const Curves *const *curves = std::get_if<Curves *>(&data)) {
    r_context.emplace(**curves, bke::AttrDomain::Point);
    return (*curves)->geometry.curve_num;
  }
  return 0;
}

void ConstraintSet::ensure_init(MutableSpan<SimGeometry> /*sim_geometries*/) {}

void ConstraintSet::solve(ConstraintSetSolveParams & /*params*/) {}

void ConstraintSet::post_solve_apply(MutableSpan<SimGeometry> /*sim_geometries*/) {}

LocalConstraintCorrections &ConstraintCorrections::local()
{
  return local_corrections_;
}

LocalConstraintCorrections::LocalConstraintCorrections(ConstraintCorrections &corrections)
    : corrections_(corrections)
{
}

ConstraintCorrections::ConstraintCorrections(MutableSpan<SimGeometry> sim_geometries)
    : sim_geometries_(sim_geometries), local_corrections_(*this)
{
  corrections_.reinitialize(sim_geometries.size());
  for (const int geometry_i : sim_geometries.index_range()) {
    corrections_[geometry_i].reinitialize(sim_geometries[geometry_i].points_num());
  }
}

void ConstraintCorrections::apply()
{
  threading::parallel_for(
      sim_geometries_.index_range(), 1, [&](const IndexRange sim_geometries_range) {
        for (const int geometry_i : sim_geometries_range) {
          SimGeometry &sim_geometry = sim_geometries_[geometry_i];
          std::optional<bke::MutableAttributeAccessor> attributes =
              sim_geometry.attributes_for_write();
          if (!attributes) {
            continue;
          }
          MutableSpan<PositionCorrection> corrections = corrections_[geometry_i];
          bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
              "position");

          threading::parallel_for(
              positions.span.index_range(), 512, [&](const IndexRange points_range) {
                const float quantize_scale = sim_geometry.quantize_scale;
                for (const int point_i : points_range) {
                  const PositionCorrection &correction = corrections[point_i];
                  if (correction.num_corrections == 0) {
                    continue;
                  }
                  const int3 offset_quantized = correction.offset;
                  const float factor = 1.0f / (quantize_scale * float(correction.num_corrections));
                  const float3 offset = float3(offset_quantized) * factor;
                  float3 &position = positions.span[point_i];
                  position += offset;
                }
              });

          positions.finish();
        }
      });
}

class EdgeLengthConstraint : public ConstraintSet {
 private:
  std::string rest_length_attribute_;
  float compliance_;

 public:
  EdgeLengthConstraint(std::string rest_length_attribute, const float compliance)
      : rest_length_attribute_(std::move(rest_length_attribute)), compliance_(compliance)
  {
  }

  void ensure_init(MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      Mesh **mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      Mesh &mesh = **mesh_ptr;
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      if (attributes.contains(rest_length_attribute_)) {
        continue;
      }

      float *rest_lengths = MEM_malloc_arrayN<float>(mesh.edges_num, __func__);
      const Span<float3> positions = mesh.vert_positions();
      const Span<int2> edges = mesh.edges();
      threading::parallel_for(IndexRange(mesh.edges_num), 512, [&](const IndexRange range) {
        for (const int i : range) {
          const int2 edge = edges[i];
          const float length = math::distance(positions[edge[0]], positions[edge[1]]);
          rest_lengths[i] = length;
        }
      });
      attributes.add<float>(rest_length_attribute_,
                            bke::AttrDomain::Edge,
                            bke::AttributeInitMoveArray{rest_lengths});
    }
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_adder = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_adder = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      const Mesh *const *mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      const Mesh &mesh = **mesh_ptr;
      const bke::AttributeAccessor attributes = mesh.attributes();
      const Span<int2> edges = mesh.edges();
      const Span<float3> positions = mesh.vert_positions();
      const bke::AttributeReader<float> rest_lengths = attributes.lookup<float>(
          rest_length_attribute_, bke::AttrDomain::Edge);
      if (!rest_lengths) {
        continue;
      }
      const bke::AttributeReader<float> masses = attributes.lookup_or_default<float>(
          sim_geometry.mass_attribute, bke::AttrDomain::Point, 1.0f);
      if (!masses) {
        continue;
      }
      threading::parallel_for(IndexRange(mesh.edges_num), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int edge_i : range) {
          const int2 edge = edges[edge_i];
          const int i0 = edge[0];
          const int i1 = edge[1];
          const float3 &p0 = positions[i0];
          const float3 &p1 = positions[i1];
          const float m0 = masses.varray[i0];
          const float m1 = masses.varray[i1];

          const float3 p_diff = p1 - p0;
          float length;
          const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);

          float length_diff = length - rest_lengths.varray[edge_i];
          const float lambda = length_diff / (1.0f / m0 + 1.0f / m1 + compliance_adder);

          const float m_sum = m0 + m1;
          const float3 correction0 = lambda * m0 / m_sum * normalized_dir;
          const float3 correction1 = -lambda * m1 / m_sum * normalized_dir;
          local_corrections.add_position_correction(geometry_i, i0, correction0);
          local_corrections.add_position_correction(geometry_i, i1, correction1);
        }
      });
    }
  }
};

class FixedPositionsConstraint : public ConstraintSet {
 private:
  fn::Field<bool> selection_field_;
  fn::Field<float3> fixed_positions_field_;

 public:
  FixedPositionsConstraint(fn::Field<bool> selection_field,
                           fn::Field<float3> fixed_positions_field)
      : selection_field_(std::move(selection_field)),
        fixed_positions_field_(std::move(fixed_positions_field))
  {
  }

  void post_solve_apply(MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      std::optional<bke::GeometryFieldContext> field_context;
      const int points_num = sim_geometry.set_point_field_context(field_context);
      if (!field_context) {
        continue;
      }
      fn::FieldEvaluator field_evaluator{*field_context, points_num};
      field_evaluator.set_selection(selection_field_);
      field_evaluator.add(fixed_positions_field_);
      field_evaluator.evaluate();
      const IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
      if (selection.is_empty()) {
        continue;
      }
      const VArraySpan<float3> fixed_positions_span = field_evaluator.get_evaluated<float3>(0);
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
          "position");
      selection.foreach_index([&](const int i) { positions.span[i] = fixed_positions_span[i]; });
      positions.finish();
    }
  }
};

class InfiniteCollisionPlaneConstraint : public ConstraintSet {
 private:
  float3 position_;
  float3 normal_;

 public:
  InfiniteCollisionPlaneConstraint(const float3 &position, const float3 &normal)
      : position_(position), normal_(math::normalize(normal))
  {
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
      if (!attributes) {
        continue;
      }
      const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
      threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int i : range) {
          const float3 &position = positions[i];
          const float distance = math::dot(position - position_, normal_);
          if (distance > 0.0f) {
            continue;
          }
          local_corrections.add_position_correction(geometry_i, i, normal_ * -distance);
        }
      });
    }
  }
};

ConstraintSet &create_constraint__edge_lengths(ResourceScope &scope,
                                               std::string rest_length_attribute,
                                               const float compliance)
{
  return scope.construct<EdgeLengthConstraint>(std::move(rest_length_attribute), compliance);
}

ConstraintSet &create_constraint__fixed_positions(ResourceScope &scope,
                                                  fn::Field<bool> selection_field,
                                                  fn::Field<float3> fixed_positions_field)
{
  return scope.construct<FixedPositionsConstraint>(std::move(selection_field),
                                                   std::move(fixed_positions_field));
}

ConstraintSet &create_constraint__infinite_collision_plane(ResourceScope &scope,
                                                           const float3 &position,
                                                           const float3 &normal)
{
  return scope.construct<InfiniteCollisionPlaneConstraint>(position, normal);
}

void solve(Behaviors &behaviors, const float total_delta_time, const int substeps)
{
  BLI_assert(total_delta_time >= 0.0f);
  BLI_assert(substeps >= 0);
  const int sim_steps = 1 + substeps;
  const float sub_delta_time = total_delta_time / sim_steps;

  Vector<SimGeometry> sim_geometries;
  for (SimGeometrySet &sim_geometry_set : behaviors.sim_geometry_sets) {
    if (Mesh *mesh = sim_geometry_set.geometry.get_mesh_for_write()) {
      sim_geometries.append(SimGeometry{sim_geometry_set, mesh});
    }
    if (PointCloud *pointcloud = sim_geometry_set.geometry.get_pointcloud_for_write()) {
      sim_geometries.append(SimGeometry{sim_geometry_set, pointcloud});
    }
    if (Curves *curves = sim_geometry_set.geometry.get_curves_for_write()) {
      sim_geometries.append(SimGeometry{sim_geometry_set, curves});
    }
  }

  for ([[maybe_unused]] int substep : IndexRange(sim_steps)) {
    /* Remember previous positions. */
    for (SimGeometry &sim_geometry : sim_geometries) {
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      const bke::AttributeReader<float3> positions = attributes->lookup<float3>("position");
      attributes->remove(prev_position_name);
      attributes->add<float3>(prev_position_name,
                              bke::AttrDomain::Point,
                              bke::AttributeInitShared{positions.varray.get_internal_span().data(),
                                                       *positions.sharing_info});
    }

    /* Init constraints. */
    for (ConstraintSet *constraint : behaviors.constraint_sets) {
      constraint->ensure_init(sim_geometries);
    }

    /* Handle forces and accelerations. If the time step is zero, these can't have any effect. */
    if (sub_delta_time > 0) {
      for (SimGeometry &sim_geometry : sim_geometries) {
        std::optional<bke::MutableAttributeAccessor> attributes =
            sim_geometry.attributes_for_write();
        if (!attributes) {
          continue;
        }
        std::optional<bke::GeometryFieldContext> field_context;
        sim_geometry.set_point_field_context(field_context);
        if (!field_context) {
          continue;
        }
        const int positions_num = attributes->domain_size(bke::AttrDomain::Point);
        Array<float3> force(positions_num, float3());
        Array<float3> acceleration(positions_num, float3());
        fn::FieldEvaluator field_evaluator{*field_context, positions_num};
        for (const ForceField &sim_force : behaviors.force_fields) {
          field_evaluator.add(sim_force.force_field);
        }
        for (const AccelerationField &sim_acceleration : behaviors.acceleration_fields) {
          field_evaluator.add(sim_acceleration.acceleration_field);
        }
        field_evaluator.evaluate();
        for (const int force_i : behaviors.force_fields.index_range()) {
          VArraySpan<float3> force_varray = field_evaluator.get_evaluated<float3>(force_i);
          for (const int i : force_varray.index_range()) {
            force[i] += force_varray[i];
          }
        }
        for (const int acceleration_i : behaviors.acceleration_fields.index_range()) {
          VArraySpan<float3> acceleration_varray = field_evaluator.get_evaluated<float3>(
              acceleration_i + behaviors.force_fields.size());
          for (const int i : acceleration_varray.index_range()) {
            acceleration[i] += acceleration_varray[i];
          }
        }
        const VArray<float> masses = *attributes->lookup_or_default<float>(
            sim_geometry.mass_attribute, bke::AttrDomain::Point, 1.0f);

        bke::SpanAttributeWriter<float3> velocities =
            attributes->lookup_or_add_for_write_span<float3>(sim_geometry.velocity_attribute,
                                                             bke::AttrDomain::Point);
        bke::SpanAttributeWriter<float3> positions =
            attributes->lookup_or_add_for_write_span<float3>("position", bke::AttrDomain::Point);
        for (const int i : velocities.span.index_range()) {
          acceleration[i] += force[i] * sub_delta_time / masses[i];
          velocities.span[i] += acceleration[i] * sub_delta_time;
          positions.span[i] += velocities.span[i] * sub_delta_time;
        }
        velocities.finish();
        positions.finish();
      }
    }

    /* Constraint solve step. */
    ConstraintCorrections corrections(sim_geometries);
    ConstraintSetSolveParams params{sub_delta_time, sim_geometries, corrections};
    threading::parallel_for(
        behaviors.constraint_sets.index_range(), 1, [&](const IndexRange range) {
          for (const int constraint_i : range) {
            ConstraintSet *constraints = behaviors.constraint_sets[constraint_i];
            constraints->solve(params);
          }
        });
    corrections.apply();

    /* Apply hard constraints. */
    for (ConstraintSet *constraint : behaviors.constraint_sets) {
      constraint->post_solve_apply(sim_geometries);
    }

    /* Write back velocities. Velocities can't be computed if the time step is zero. */
    if (sub_delta_time > 0) {
      for (SimGeometry &sim_geometry : sim_geometries) {
        std::optional<bke::MutableAttributeAccessor> attributes =
            sim_geometry.attributes_for_write();
        if (!attributes) {
          continue;
        }
        const VArraySpan<float3> prev_positions = *attributes->lookup<float3>(prev_position_name);
        const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
        bke::SpanAttributeWriter<float3> velocities = attributes->lookup_for_write_span<float3>(
            sim_geometry.velocity_attribute);
        threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
          for (const int i : range) {
            velocities.span[i] = (positions[i] - prev_positions[i]) / sub_delta_time;
          }
        });
        velocities.finish();
      }
    }

    /* Remove temporary attributes.*/
    for (SimGeometry &sim_geometry : sim_geometries) {
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      attributes->remove(prev_position_name);
    }
  }
}

}  // namespace blender::geometry::xpbd
