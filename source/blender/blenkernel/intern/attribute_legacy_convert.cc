/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define DNA_DEPRECATED_ALLOW

#include <optional>

#include "BLI_string.h"

#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_customdata.hh"

#include "BKE_attribute_legacy_convert.hh"

namespace blender::bke {

static std::optional<AttrType> custom_data_type_to_attribute_type(const eCustomDataType data_type)
{
  switch (data_type) {
    case CD_AUTO_FROM_NAME:
    case CD_MVERT:
    case CD_MSTICKY:
    case CD_MEDGE:
    case CD_FACEMAP:
    case CD_MTEXPOLY:
    case CD_MLOOPUV:
    case CD_MPOLY:
    case CD_MLOOP:
    case CD_SHAPE_KEYINDEX:
    case CD_SHAPEKEY:
    case CD_BWEIGHT:
    case CD_CREASE:
    case CD_BM_ELEM_PYPTR:
    case CD_PAINT_MASK:
    case CD_CUSTOMLOOPNORMAL:
    case CD_SCULPT_FACE_SETS:
    case CD_NUMTYPES:
      BLI_assert_unreachable();
      return std::nullopt;
    case CD_MDEFORMVERT:
    case CD_MFACE:
    case CD_MTFACE:
    case CD_MCOL:
    case CD_ORIGINDEX:
    case CD_NORMAL:
    case CD_ORIGSPACE:
    case CD_ORCO:
    case CD_TANGENT:
    case CD_MDISPS:
    case CD_CLOTH_ORCO:
    case CD_ORIGSPACE_MLOOP:
    case CD_GRID_PAINT_MASK:
    case CD_MVERT_SKIN:
    case CD_FREESTYLE_EDGE:
    case CD_FREESTYLE_FACE:
    case CD_MLOOPTANGENT:
    case CD_TESSLOOPNORMAL:
      return std::nullopt;
    case CD_PROP_FLOAT:
      return AttrType::Float;
    case CD_PROP_INT32:
      return AttrType::Int32;
    case CD_PROP_BYTE_COLOR:
      return AttrType::ColorByte;
    case CD_PROP_FLOAT4X4:
      return AttrType::Float4x4;
    case CD_PROP_INT16_2D:
      return AttrType::Int16_2D;
    case CD_PROP_INT8:
      return AttrType::Int8;
    case CD_PROP_INT32_2D:
      return AttrType::Int32_2D;
    case CD_PROP_COLOR:
      return AttrType::ColorFloat;
    case CD_PROP_FLOAT3:
      return AttrType::Float3;
    case CD_PROP_FLOAT2:
      return AttrType::Float2;
    case CD_PROP_BOOL:
      return AttrType::Bool;
    case CD_PROP_STRING:
      return AttrType::String;
    case CD_PROP_QUATERNION:
      return AttrType::Quaternion;
  }
  BLI_assert_unreachable();
  return std::nullopt;
}

AttributeStorage attribute_legacy_convert_customdata_to_storage(
    const Map<AttrDomain, std::pair<CustomData *, int>> &domains)
{
  AttributeStorage storage{};
  struct AttributeToAdd {
    StringRef name;
    AttrDomain domain;
    AttrType type;
    void *array_data;
    int array_size;
    const ImplicitSharingInfo *sharing_info;
  };
  Vector<AttributeToAdd> attributes_to_add;
  for (const auto &item : domains.items()) {
    CustomData &custom_data = *item.value.first;
    Vector<CustomDataLayer> kept_layers;
    for (CustomDataLayer &layer : MutableSpan(custom_data.layers, custom_data.totlayer)) {
      if (std::optional<AttrType> attr_type = custom_data_type_to_attribute_type(
              eCustomDataType(layer.type)))
      {
        attributes_to_add.append(
            {layer.name, item.key, *attr_type, layer.data, item.value.second, layer.sharing_info});
        layer.data = nullptr;
        layer.sharing_info = nullptr;
      }
      else {
        kept_layers.append(layer);
      }
    }
    VectorData<CustomDataLayer, GuardedAllocator> kept_layers_data = kept_layers.release();
    custom_data.layers = kept_layers_data.data;
    custom_data.totlayer = kept_layers_data.size;
    custom_data.maxlayer = kept_layers_data.capacity;
  }

  storage.attributes_array = static_cast<::Attribute **>(
      MEM_malloc_arrayN(attributes_to_add.size(), sizeof(::Attribute *), __func__));
  storage.attributes_num = attributes_to_add.size();
  storage.attributes_capacity = attributes_to_add.size();

  for (const int i : attributes_to_add.index_range()) {
    AttributeToAdd &src = attributes_to_add[i];
    Attribute *dst = MEM_cnew<Attribute>(__func__);
    storage.attributes_array[i] = dst;

    dst->name = BLI_strdupn(src.name.data(), src.name.size());
    dst->domain = int16_t(src.domain);
    dst->data_type = int16_t(src.type);
    dst->storage_type = int8_t(AttrStorageType::Array);
    AttributeDataArray *data = MEM_cnew<AttributeDataArray>(__func__);
    dst->data = data;
    data->data = src.array_data;
    data->sharing_info = src.sharing_info;
    data->elements_num = src.array_size;
  }
  return storage;
}

static std::optional<eCustomDataType> attribute_to_to_custom_data_type(const AttrType attr_type)
{
  switch (attr_type) {
    case AttrType::Bool:
      return CD_PROP_BOOL;
    case AttrType::Int8:
      return CD_PROP_INT8;
    case AttrType::Int16_2D:
      return CD_PROP_INT16_2D;
    case AttrType::Int32:
      return CD_PROP_INT32;
    case AttrType::Int32_2D:
      return CD_PROP_INT32_2D;
    case AttrType::Float:
      return CD_PROP_FLOAT;
    case AttrType::Float2:
      return CD_PROP_FLOAT2;
    case AttrType::Float3:
      return CD_PROP_FLOAT3;
    case AttrType::Float4x4:
      return CD_PROP_FLOAT4X4;
    case AttrType::ColorByte:
      return CD_PROP_BYTE_COLOR;
    case AttrType::ColorFloat:
      return CD_PROP_COLOR;
    case AttrType::Quaternion:
      return CD_PROP_QUATERNION;
    case AttrType::String:
      return CD_PROP_STRING;
  }
  return std::nullopt;
}

void attribute_legacy_convert_storage_to_customdata(
    AttributeStorage &storage,
    const Map<AttrDomain, std::pair<CustomData *, int>> &custom_data_domains)
{
  for (::Attribute *attribute : Span(storage.attributes_array, storage.attributes_num)) {
    if (AttrStorageType(attribute->storage_type) != AttrStorageType::Array) {
      continue;
    }
    const std::optional<eCustomDataType> data_type = attribute_to_to_custom_data_type(
        AttrType(attribute->data_type));
    if (!data_type) {
      continue;
    }
    const auto &array_data = *static_cast<const AttributeDataArray *>(attribute->data);
    BLI_assert(array_data.elements_num ==
               custom_data_domains.lookup(AttrDomain(attribute->domain)).second);
    CustomData_add_layer_named_with_data(
        custom_data_domains.lookup(AttrDomain(attribute->domain)).first,
        *data_type,
        attribute->data,
        array_data.elements_num,
        attribute->name,
        array_data.sharing_info);
  }
}

static auto mesh_domains(Mesh &mesh)
{
  return Map<AttrDomain, std::pair<CustomData *, int>>{
      {AttrDomain::Point, {&mesh.vert_data, mesh.verts_num}},
      {AttrDomain::Edge, {&mesh.edge_data, mesh.edges_num}},
      {AttrDomain::Face, {&mesh.face_data, mesh.faces_num}},
      {AttrDomain::Corner, {&mesh.corner_data, mesh.corners_num}}};
}

void mesh_convert_storage_to_customdata(Mesh &mesh)
{
  attribute_legacy_convert_storage_to_customdata(mesh.attribute_storage.wrap(),
                                                 mesh_domains(mesh));
}
void mesh_convert_customdata_to_storage(Mesh &mesh)
{
  mesh.attribute_storage.wrap() = bke::attribute_legacy_convert_customdata_to_storage(
      mesh_domains(mesh));
}

static auto curves_domains(CurvesGeometry &curves)
{
  return Map<AttrDomain, std::pair<CustomData *, int>>{
      {AttrDomain::Point, {&curves.point_data, curves.points_num()}},
      {AttrDomain::Curve, {&curves.curve_data, curves.curves_num()}}};
}

void curves_convert_storage_to_customdata(CurvesGeometry &curves)
{
  attribute_legacy_convert_storage_to_customdata(curves.attribute_storage.wrap(),
                                                 curves_domains(curves));
}
void curves_convert_customdata_to_storage(CurvesGeometry &curves)
{
  curves.attribute_storage.wrap() = bke::attribute_legacy_convert_customdata_to_storage(
      curves_domains(curves));
}

static auto pointcloud_domains(PointCloud &pointcloud)
{
  return Map<AttrDomain, std::pair<CustomData *, int>>{
      {AttrDomain::Point, {&pointcloud.pdata, pointcloud.totpoint}}};
}

void pointcloud_convert_storage_to_customdata(PointCloud &pointcloud)
{
  attribute_legacy_convert_storage_to_customdata(pointcloud.attribute_storage.wrap(),
                                                 pointcloud_domains(pointcloud));
}
void pointcloud_convert_customdata_to_storage(PointCloud &pointcloud)
{
  pointcloud.attribute_storage.wrap() = bke::attribute_legacy_convert_customdata_to_storage(
      pointcloud_domains(pointcloud));
}

static auto grease_pencil_domains(GreasePencil &grease_pencil)
{
  return Map<AttrDomain, std::pair<CustomData *, int>>{
      {AttrDomain::Layer, {&grease_pencil.layers_data, grease_pencil.layers().size()}}};
}

void grease_pencil_convert_storage_to_customdata(GreasePencil &grease_pencil)
{
  attribute_legacy_convert_storage_to_customdata(grease_pencil.attribute_storage.wrap(),
                                                 grease_pencil_domains(grease_pencil));
}
void grease_pencil_convert_customdata_to_storage(GreasePencil &grease_pencil)
{
  grease_pencil.attribute_storage.wrap() = bke::attribute_legacy_convert_customdata_to_storage(
      grease_pencil_domains(grease_pencil));
}

}  // namespace blender::bke
