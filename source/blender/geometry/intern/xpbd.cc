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

}  // namespace blender::geometry::xpbd
