/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"

#include "DNA_attribute_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"

struct CustomData;
namespace blender::bke {
class CurvesGeometry;
}
struct PointCloud;
struct GreasePencil;
struct Mesh;

namespace blender::bke {

[[nodiscard]] AttributeStorage attribute_legacy_convert_customdata_to_storage(
    const Map<AttrDomain, std::pair<CustomData *, int>> &domains);

void attribute_legacy_convert_storage_to_customdata(
    AttributeStorage &storage, const Map<AttrDomain, std::pair<CustomData *, int>> &domains);

void mesh_convert_storage_to_customdata(Mesh &mesh);
void mesh_convert_customdata_to_storage(Mesh &mesh);

void curves_convert_storage_to_customdata(CurvesGeometry &curves);
void curves_convert_customdata_to_storage(CurvesGeometry &curves);

void pointcloud_convert_storage_to_customdata(PointCloud &pointcloud);
void pointcloud_convert_customdata_to_storage(PointCloud &pointcloud);

void grease_pencil_convert_storage_to_customdata(GreasePencil &grease_pencil);
void grease_pencil_convert_customdata_to_storage(GreasePencil &grease_pencil);

}  // namespace blender::bke
