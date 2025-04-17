/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

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

std::optional<AttrType> custom_data_type_to_attr_type(eCustomDataType data_type);
std::optional<eCustomDataType> attr_type_to_custom_data_type(AttrType attr_type);

void mesh_convert_storage_to_customdata(Mesh &mesh);
AttributeStorage mesh_convert_customdata_to_storage(const Mesh &mesh);

void curves_convert_storage_to_customdata(CurvesGeometry &curves);
AttributeStorage curves_convert_customdata_to_storage(const CurvesGeometry &curves);

void pointcloud_convert_storage_to_customdata(PointCloud &pointcloud);
AttributeStorage pointcloud_convert_customdata_to_storage(const PointCloud &pointcloud);

void grease_pencil_convert_storage_to_customdata(GreasePencil &grease_pencil);
AttributeStorage grease_pencil_convert_customdata_to_storage(const GreasePencil &grease_pencil);

}  // namespace blender::bke
