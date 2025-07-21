/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"
#include "GEO_xpbd.hh"

namespace blender::geometry::xpbd {

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

void XpbdConstraintCorrections::apply(MutableSpan<SimGeometry> sim_geometries)
{
  Vector<bke::SpanAttributeWriter<float3>> position_attributes(sim_geometries.size());
  for (const int geometry_i : sim_geometries.index_range()) {
    SimGeometry &sim_geometry = sim_geometries[geometry_i];
    std::optional<bke::MutableAttributeAccessor> attributes = sim_geometry.attributes_for_write();
    if (!attributes) {
      continue;
    }
    position_attributes[geometry_i] = attributes->lookup_for_write_span<float3>("position");
  }
  Map<std::pair<int, int>, int> num_corrections_map;
  for (LocalXpbdConstraintCorrections &local_corrections : local_corrections_) {
    for (const PositionCorrection &correction : local_corrections.position_corrections_) {
      num_corrections_map.lookup_or_add({correction.geometry_i, correction.position_i}, 0) += 1;
    }
  }
  for (LocalXpbdConstraintCorrections &local_corrections : local_corrections_) {
    for (const PositionCorrection &correction : local_corrections.position_corrections_) {
      const int num_corrections = num_corrections_map.lookup(
          {correction.geometry_i, correction.position_i});
      position_attributes[correction.geometry_i].span[correction.position_i] +=
          correction.correction / num_corrections;
    }
  }
  for (bke::SpanAttributeWriter<float3> &attribute : position_attributes) {
    attribute.finish();
  }
}

class EdgeLengthConstraint : public XpbdContraints {
 private:
  std::string rest_length_attribute_;

 public:
  EdgeLengthConstraint(std::string rest_length_attribute)
      : rest_length_attribute_(std::move(rest_length_attribute))
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

  void solve(const Span<SimGeometry> sim_geometries,
             XpbdConstraintCorrections &corrections) override
  {
    for (const int geometry_i : sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = sim_geometries[geometry_i];
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
        LocalXpbdConstraintCorrections &local_corrections = corrections.local();
        for (const int edge_i : range) {
          const int2 edge = edges[edge_i];
          const int i0 = edge[0];
          const int i1 = edge[1];
          const float3 &p0 = positions[i0];
          const float3 &p1 = positions[i1];
          const float3 p_diff = p1 - p0;
          float length;
          const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);
          float length_diff = length - rest_lengths.varray[edge_i];
          const float m0 = masses.varray[i0];
          const float m1 = masses.varray[i1];
          const float m_sum = m0 + m1;
          const float3 correction0 = m0 / m_sum * length_diff * normalized_dir;
          const float3 correction1 = -m1 / m_sum * length_diff * normalized_dir;
          local_corrections.add_position_correction(geometry_i, i0, correction0);
          local_corrections.add_position_correction(geometry_i, i1, correction1);
        }
      });
    }
  }
};

class FixedPositionsConstraint : public XpbdContraints {
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

XpbdContraints &create_constraint__edge_lengths(ResourceScope &scope,
                                                std::string rest_length_attribute)
{
  return scope.construct<EdgeLengthConstraint>(std::move(rest_length_attribute));
}

XpbdContraints &create_constraint__fixed_positions(ResourceScope &scope,
                                                   fn::Field<bool> selection_field,
                                                   fn::Field<float3> fixed_positions_field)
{
  return scope.construct<FixedPositionsConstraint>(std::move(selection_field),
                                                   std::move(fixed_positions_field));
}

}  // namespace blender::geometry::xpbd
