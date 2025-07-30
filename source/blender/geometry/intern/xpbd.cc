/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"
#include "GEO_xpbd.hh"

#include "NOD_geometry_nodes_behaviors.hh"
#include "NOD_geometry_nodes_bundle.hh"

namespace blender::geometry::xpbd {

using nodes::behaviors::behavior_path_is_selected;

constexpr StringRefNull prev_position_name = ".prev_position";

SimGeometry::SimGeometry(SimGeometrySet &src, GeometryVariant data) : data(data), src(src) {}

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
    return (*curves)->geometry.point_num;
  }
  return 0;
}

nodes::Bundle &SimGeometrySet::extra_for_write()
{
  /* Ensure the caller locked it already. */
  BLI_assert(!this->extra_mutex.try_lock());
  if (!this->extra) {
    this->extra = nodes::Bundle::create();
  }
  else if (!this->extra->is_mutable()) {
    this->extra = this->extra->copy();
    this->extra->tag_ensured_mutable();
  }
  return const_cast<nodes::Bundle &>(*this->extra);
}

template<typename T> void SimGeometrySet::set_extra(const StringRef key, T value)
{
  std::scoped_lock lock(this->extra_mutex);
  nodes::Bundle &extra = this->extra_for_write();
  extra.add_override<std::decay_t<T>>(key, std::move(value));
}

template<typename T> std::optional<T> SimGeometrySet::get_extra(const StringRef key) const
{
  std::scoped_lock lock(this->extra_mutex);
  if (!this->extra) {
    return std::nullopt;
  }
  return this->extra->lookup<T>(key);
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
                  const float relaxation_factor = 1.3f;
                  const float factor = relaxation_factor /
                                       (quantize_scale * float(correction.num_corrections));
                  const float3 offset = float3(offset_quantized) * factor;
                  float3 &position = positions.span[point_i];
                  position += offset;
                }
              });

          positions.finish();
        }
      });
}

static void solve_distance_constraint(const int geometry0,
                                      const int geometry1,
                                      const int i0,
                                      const int i1,
                                      const float3 &p0,
                                      const float3 &p1,
                                      const float m0,
                                      const float m1,
                                      const float compliance_term,
                                      const float rest_distance,
                                      LocalConstraintCorrections &local_corrections)
{
  const float3 p_diff = p1 - p0;
  float length;
  const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);

  float length_diff = length - rest_distance;
  const float lambda = length_diff / (1.0f / m0 + 1.0f / m1 + compliance_term);

  const float m_sum = m0 + m1;
  const float3 correction0 = lambda * m0 / m_sum * normalized_dir;
  const float3 correction1 = -lambda * m1 / m_sum * normalized_dir;
  local_corrections.add_position_correction(geometry0, i0, correction0);
  local_corrections.add_position_correction(geometry1, i1, correction1);
}

class EdgeLengthConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  std::string rest_length_attribute_;
  float compliance_;

 public:
  EdgeLengthConstraintSet(std::string self_path,
                          std::string filter,
                          std::string rest_length_attribute,
                          const float compliance)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        rest_length_attribute_(std::move(rest_length_attribute)),
        compliance_(compliance)
  {
  }

  void ensure_init(MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
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
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
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
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
      if (!masses) {
        continue;
      }
      threading::parallel_for(IndexRange(mesh.edges_num), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int edge_i : range) {
          const int2 edge = edges[edge_i];
          const int i0 = edge[0];
          const int i1 = edge[1];
          solve_distance_constraint(geometry_i,
                                    geometry_i,
                                    i0,
                                    i1,
                                    positions[i0],
                                    positions[i1],
                                    masses.varray[i0],
                                    masses.varray[i1],
                                    compliance_term,
                                    rest_lengths.varray[edge_i],
                                    local_corrections);
        }
      });
    }
  }
};

class CurveLengthConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  std::string rest_length_attribute_;
  float compliance_;

 public:
  CurveLengthConstraintSet(std::string self_path,
                           std::string filter,
                           std::string rest_length_attribute,
                           const float compliance)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        rest_length_attribute_(std::move(rest_length_attribute)),
        compliance_(compliance)
  {
  }

  void ensure_init(MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Curves **curves_ptr = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_ptr) {
        continue;
      }
      Curves &curves_id = **curves_ptr;
      bke::CurvesGeometry &curves = curves_id.geometry.wrap();
      bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
      if (attributes.contains(rest_length_attribute_)) {
        continue;
      }
      float *rest_lengths = MEM_malloc_arrayN<float>(curves.points_num(), __func__);
      const Span<float3> positions = curves.positions();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      threading::parallel_for(curves.curves_range(), 256, [&](IndexRange curves_range) {
        for (const int curve_i : curves_range) {
          const IndexRange points = points_by_curve[curve_i];
          if (points.size() < 2) {
            continue;
          }
          for (const int point_i : points.drop_back(1)) {
            const int next_point_i = point_i + 1;
            const float3 &p0 = positions[point_i];
            const float3 &p1 = positions[next_point_i];
            const float length = math::distance(p0, p1);
            rest_lengths[point_i] = length;
          }
          /* Cyclic segment length. Not strictly necessary to compute in all cases. */
          rest_lengths[points.last()] = math::distance(positions[points.last()], positions[0]);
        }
      });
      attributes.add<float>(rest_length_attribute_,
                            bke::AttrDomain::Point,
                            bke::AttributeInitMoveArray{rest_lengths});
    }
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Curves *const *curves_id = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_id) {
        continue;
      }
      const bke::CurvesGeometry &curves = (**curves_id).geometry.wrap();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      const Span<float3> positions = curves.positions();
      const bke::AttributeAccessor attributes = curves.attributes();
      const bke::AttributeReader<float> rest_lengths = attributes.lookup<float>(
          rest_length_attribute_, bke::AttrDomain::Point);
      if (!rest_lengths) {
        continue;
      }
      const bke::AttributeReader<float> masses = attributes.lookup_or_default<float>(
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
      if (!masses) {
        continue;
      }
      const VArray<bool> cyclic = curves.cyclic();
      threading::parallel_for(curves.curves_range(), 256, [&](IndexRange curves_range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int curve_i : curves_range) {
          const IndexRange points = points_by_curve[curve_i];
          if (points.size() < 2) {
            continue;
          }
          for (const int point_i : points.drop_back(1)) {
            const int next_point_i = point_i + 1;
            solve_distance_constraint(geometry_i,
                                      geometry_i,
                                      point_i,
                                      next_point_i,
                                      positions[point_i],
                                      positions[next_point_i],
                                      masses.varray[point_i],
                                      masses.varray[next_point_i],
                                      compliance_term,
                                      rest_lengths.varray[point_i],
                                      local_corrections);
          }
          if (cyclic[curve_i]) {
            const int first_point_i = points.first();
            const int last_point_i = points.last();
            solve_distance_constraint(geometry_i,
                                      geometry_i,
                                      first_point_i,
                                      last_point_i,
                                      positions[first_point_i],
                                      positions[last_point_i],
                                      masses.varray[first_point_i],
                                      masses.varray[last_point_i],
                                      compliance_term,
                                      rest_lengths.varray[last_point_i],
                                      local_corrections);
          }
        }
      });
    }
  }
};

class FixedPositionsConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  fn::Field<bool> selection_field_;
  fn::Field<float3> fixed_positions_field_;

 public:
  FixedPositionsConstraintSet(std::string self_path,
                              std::string filter,
                              fn::Field<bool> selection_field,
                              fn::Field<float3> fixed_positions_field)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        selection_field_(std::move(selection_field)),
        fixed_positions_field_(std::move(fixed_positions_field))
  {
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    this->foreach_fixed_position(
        params.sim_geometries,
        [&](const int geometry_i, const IndexMask &mask, const VArray<float3> &fixed_positions) {
          const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
          std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
          if (!attributes) {
            return;
          }
          const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
          LocalConstraintCorrections &local_corrections = params.corrections.local();
          mask.foreach_index([&](const int i) {
            const float3 offset = fixed_positions[i] - positions[i];
            local_corrections.add_position_correction(geometry_i, i, offset);
          });
        });
  }

  void post_solve_apply(MutableSpan<SimGeometry> sim_geometries) override
  {
    this->foreach_fixed_position(
        sim_geometries,
        [&](const int geometry_i, const IndexMask &mask, const VArray<float3> &fixed_positions) {
          SimGeometry &sim_geometry = sim_geometries[geometry_i];
          std::optional<bke::MutableAttributeAccessor> attributes =
              sim_geometry.attributes_for_write();
          if (!attributes) {
            return;
          }
          bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
              "position");
          mask.foreach_index([&](const int i) { positions.span[i] = fixed_positions[i]; });
          positions.finish();
        });
  }

  void foreach_fixed_position(const Span<SimGeometry> sim_geometries,
                              FunctionRef<void(const int geometry_i,
                                               const IndexMask &mask,
                                               const VArray<float3> &fixed_positions)> fn) const
  {
    for (const int geometry_i : sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = sim_geometries[geometry_i];
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
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
      const VArray<float3> fixed_positions = field_evaluator.get_evaluated<float3>(0);
      fn(geometry_i, selection, fixed_positions);
    }
  }
};

class InfiniteCollisionPlaneConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  float3 position_;
  float3 normal_;

 public:
  InfiniteCollisionPlaneConstraintSet(std::string self_path,
                                      std::string filter,
                                      const float3 &position,
                                      const float3 &normal)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        position_(position),
        normal_(math::normalize(normal))
  {
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
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
          if (distance >= 0.0f) {
            continue;
          }
          local_corrections.add_position_correction(geometry_i, i, normal_ * -distance);
        }
      });
    }
  }

  void post_solve_apply(MutableSpan<SimGeometry> sim_geometries) override
  {
    for (const int geometry_i : sim_geometries.index_range()) {
      SimGeometry &sim_geometry = sim_geometries[geometry_i];
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
          "position");
      threading::parallel_for(positions.span.index_range(), 512, [&](const IndexRange range) {
        for (const int i : range) {
          float3 &position = positions.span[i];
          const float distance = math::dot(position - position_, normal_);
          if (distance >= 0.0f) {
            continue;
          }
          position -= normal_ * distance;
        }
      });
      positions.finish();
    }
  }
};

class GlobalVolumeConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  std::string rest_volume_name_;
  float overpressure_;

 public:
  GlobalVolumeConstraintSet(std::string self_path,
                            std::string filter,
                            std::string rest_volume_name,
                            const float overpressure = 1.0f)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        rest_volume_name_(std::move(rest_volume_name)),
        overpressure_(overpressure)
  {
  }

  void ensure_init(MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Mesh **mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      const Mesh &mesh = **mesh_ptr;
      if (sim_geometry.src.get_extra<float>(rest_volume_name_)) {
        continue;
      }
      const float volume = this->compute_volume(mesh);
      sim_geometry.src.set_extra<float>(rest_volume_name_, volume);
    }
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!behavior_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Mesh *const *mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      const Mesh &mesh = **mesh_ptr;
      if (mesh.verts_num == 0) {
        continue;
      }
      const std::optional<float> rest_volume = sim_geometry.src.get_extra<float>(
          rest_volume_name_);
      if (!rest_volume) {
        continue;
      }
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray<float> masses = *attributes.lookup_or_default<float>(
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);

      const float current_volume = this->compute_volume(mesh);
      const float volume_diff = current_volume - overpressure_ * *rest_volume;

      Array<float3> gradients(mesh.verts_num, float3());
      const Span<int3> tris = mesh.corner_tris();
      const Span<float3> positions = mesh.vert_positions();
      const Span<int> corner_verts = mesh.corner_verts();
      threading::parallel_for(tris.index_range(), 512, [&](const IndexRange range) {
        for (const int tri_i : range) {
          const int3 &tri = tris[tri_i];
          const int v0 = corner_verts[tri[0]];
          const int v1 = corner_verts[tri[1]];
          const int v2 = corner_verts[tri[2]];
          const float3 &p0 = positions[v0];
          const float3 &p1 = positions[v1];
          const float3 &p2 = positions[v2];
          const float3 c_1_2 = math::cross(p1, p2);
          const float3 c_2_0 = math::cross(p2, p0);
          const float3 c_0_1 = math::cross(p0, p1);
          const float3 c = c_1_2 + c_2_0 + c_0_1;
          gradients[v0] += c;
          gradients[v1] += c;
          gradients[v2] += c;
        }
      });

      float lambda_divisor = 0.0f;
      for (const int i : IndexRange(mesh.verts_num)) {
        lambda_divisor += math::length_squared(gradients[i]) / masses[i];
      }
      const float lambda = math::safe_divide(volume_diff, lambda_divisor);

      threading::parallel_for(IndexRange(mesh.verts_num), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int i : range) {
          const float3 offset = -lambda / masses[i] * gradients[i];
          local_corrections.add_position_correction(geometry_i, i, offset);
        }
      });
    }
  }

  float compute_volume(const Mesh &mesh) const
  {
    const Span<int3> tris = mesh.corner_tris();
    const Span<int> corner_verts = mesh.corner_verts();
    const Span<float3> positions = mesh.vert_positions();

    const float volume = threading::parallel_deterministic_reduce<float>(
        tris.index_range(),
        512,
        0.0f,
        [&](const IndexRange range, float volume) {
          for (const int tri_i : range) {
            const int3 &tri = tris[tri_i];
            const int v0 = corner_verts[tri[0]];
            const int v1 = corner_verts[tri[1]];
            const int v2 = corner_verts[tri[2]];
            const float3 &p0 = positions[v0];
            const float3 &p1 = positions[v1];
            const float3 &p2 = positions[v2];
            volume += math::dot(math::cross(p0, p1), p2);
          }
          return volume;
        },
        [&](const float a, const float b) { return a + b; });
    return volume / 6.0f;
  }
};

ConstraintSet &create_constraint__edge_lengths(ResourceScope &scope,
                                               std::string self_path,
                                               std::string filter,
                                               std::string rest_length_attribute,
                                               const float compliance)
{
  return scope.construct<EdgeLengthConstraintSet>(
      std::move(self_path), std::move(filter), std::move(rest_length_attribute), compliance);
}
ConstraintSet &create_constraint__curve_lengths(ResourceScope &scope,
                                                std::string self_path,
                                                std::string filter,
                                                std::string rest_length_attribute,
                                                float compliance)
{
  return scope.construct<CurveLengthConstraintSet>(
      self_path, filter, std::move(rest_length_attribute), compliance);
}

ConstraintSet &create_constraint__fixed_positions(ResourceScope &scope,
                                                  std::string self_path,
                                                  std::string filter,
                                                  fn::Field<bool> selection_field,
                                                  fn::Field<float3> fixed_positions_field)
{
  return scope.construct<FixedPositionsConstraintSet>(
      self_path, filter, std::move(selection_field), std::move(fixed_positions_field));
}

ConstraintSet &create_constraint__infinite_collision_plane(ResourceScope &scope,
                                                           std::string self_path,
                                                           std::string filter,
                                                           const float3 &position,
                                                           const float3 &normal)
{
  return scope.construct<InfiniteCollisionPlaneConstraintSet>(self_path, filter, position, normal);
}

ConstraintSet &create_constraint__global_volume(ResourceScope &scope,
                                                std::string self_path,
                                                std::string filter,
                                                std::string rest_volume_name,
                                                const float overpressure)
{
  return scope.construct<GlobalVolumeConstraintSet>(
      self_path, filter, std::move(rest_volume_name), overpressure);
}

void solve(Behaviors &behaviors, const float total_delta_time, const int substeps)
{
  BLI_assert(total_delta_time >= 0.0f);
  BLI_assert(substeps >= 0);
  const int sim_steps = 1 + substeps;
  const float sub_delta_time = total_delta_time / sim_steps;

  Vector<SimGeometry> sim_geometries;
  for (SimGeometrySet *sim_geometry_set : behaviors.sim_geometry_sets) {
    if (Mesh *mesh = sim_geometry_set->geometry.get_mesh_for_write()) {
      sim_geometries.append(SimGeometry{*sim_geometry_set, mesh});
    }
    if (PointCloud *pointcloud = sim_geometry_set->geometry.get_pointcloud_for_write()) {
      sim_geometries.append(SimGeometry{*sim_geometry_set, pointcloud});
    }
    if (Curves *curves = sim_geometry_set->geometry.get_curves_for_write()) {
      sim_geometries.append(SimGeometry{*sim_geometry_set, curves});
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

        Vector<const ForceField *> filtered_force_fields;
        for (const ForceField &sim_force : behaviors.force_fields) {
          if (behavior_path_is_selected(
                  sim_force.self_path, sim_force.filter, sim_geometry.src.path))
          {
            filtered_force_fields.append(&sim_force);
          }
        }
        Vector<const AccelerationField *> filtered_acceleration_fields;
        for (const AccelerationField &sim_acceleration : behaviors.acceleration_fields) {
          if (behavior_path_is_selected(
                  sim_acceleration.self_path, sim_acceleration.filter, sim_geometry.src.path))
          {
            filtered_acceleration_fields.append(&sim_acceleration);
          }
        }

        Array<float3> force(positions_num, float3());
        Array<float3> acceleration(positions_num, float3());
        fn::FieldEvaluator field_evaluator{*field_context, positions_num};
        for (const ForceField *sim_force : filtered_force_fields) {
          field_evaluator.add(sim_force->force_field);
        }
        for (const AccelerationField *sim_acceleration : filtered_acceleration_fields) {
          field_evaluator.add(sim_acceleration->acceleration_field);
        }
        field_evaluator.evaluate();
        for (const int force_i : filtered_force_fields.index_range()) {
          VArraySpan<float3> force_varray = field_evaluator.get_evaluated<float3>(force_i);
          for (const int i : force_varray.index_range()) {
            force[i] += force_varray[i];
          }
        }
        for (const int acceleration_i : filtered_acceleration_fields.index_range()) {
          VArraySpan<float3> acceleration_varray = field_evaluator.get_evaluated<float3>(
              acceleration_i + filtered_force_fields.size());
          for (const int i : acceleration_varray.index_range()) {
            acceleration[i] += acceleration_varray[i];
          }
        }
        const VArray<float> masses = *attributes->lookup_or_default<float>(
            sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);

        bke::SpanAttributeWriter<float3> velocities =
            attributes->lookup_or_add_for_write_span<float3>(sim_geometry.src.velocity_attribute,
                                                             bke::AttrDomain::Point);
        bke::SpanAttributeWriter<float3> positions =
            attributes->lookup_or_add_for_write_span<float3>("position", bke::AttrDomain::Point);
        for (const int i : velocities.span.index_range()) {
          acceleration[i] += force[i] / masses[i];
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
            sim_geometry.src.velocity_attribute);
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
