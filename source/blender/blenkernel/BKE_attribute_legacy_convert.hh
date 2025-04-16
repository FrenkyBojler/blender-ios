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

void remove_storage_layers(CustomData &custom_data);

void mesh_convert_storage_to_customdata(Mesh &mesh);
void mesh_prepare_data_for_file_write(Mesh &mesh,
                                      Vector<CustomDataLayer, 16> &vert_layers,
                                      Vector<CustomDataLayer, 16> &edge_layers,
                                      Vector<CustomDataLayer, 16> &face_layers,
                                      Vector<CustomDataLayer, 16> &corner_layers,
                                      AttributeStorage::BlendWriteData &write_data);
AttributeStorage mesh_convert_customdata_to_storage(const Mesh &mesh);

void curves_convert_storage_to_customdata(CurvesGeometry &curves);
void curves_convert_storage_to_customdata_for_file_write(
    const AttributeStorage &storage,
    Vector<CustomDataLayer, 16> &point_layers,
    Vector<CustomDataLayer, 16> &curve_layers);
AttributeStorage curves_convert_customdata_to_storage(const CurvesGeometry &curves);

void pointcloud_convert_storage_to_customdata(PointCloud &pointcloud);
void pointcloud_convert_storage_to_customdata_for_file_write(
    const AttributeStorage &storage, Vector<CustomDataLayer, 16> &point_layers);
AttributeStorage pointcloud_convert_customdata_to_storage(const PointCloud &pointcloud);

void grease_pencil_convert_storage_to_customdata(GreasePencil &grease_pencil);
void grease_pencil_convert_storage_to_customdata_for_file_write(
    const AttributeStorage &storage, Vector<CustomDataLayer, 16> &layers);
AttributeStorage grease_pencil_convert_customdata_to_storage(const GreasePencil &grease_pencil);

}  // namespace blender::bke
