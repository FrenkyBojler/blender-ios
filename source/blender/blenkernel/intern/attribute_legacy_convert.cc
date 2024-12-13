/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define DNA_DEPRECATED_ALLOW

#include <optional>

#include "BLI_string.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"

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
    const Span<std::pair<AttrDomain, const CustomData *>> custom_data_domains)
{
  AttributeStorage r_storage{};

  struct AttributeToMove {
    StringRef name;
    AttrType attr_type;
    AttrDomain domain;
    void *data;
    const ImplicitSharingInfo *sharing_info;
  };
  Vector<AttributeToMove> attributes_to_move;
  for (auto &[domain, custom_data] : custom_data_domains) {
    Vector<CustomDataLayer> kept_layers;
    for (CustomDataLayer &layer : MutableSpan(custom_data->layers, custom_data->totlayer)) {
      std::optional<AttrType> attr_type = custom_data_type_to_attribute_type(
          eCustomDataType(layer.type));
      if (attr_type) {
        attributes_to_move.append(
            {layer.name, *attr_type, domain, layer.data, layer.sharing_info});
      }
      else {
        kept_layers.append(layer);
      }
    }
    VectorData<CustomDataLayer, GuardedAllocator> kept_layers_data = kept_layers.release();
    custom_data->layers = kept_layers_data.data;
    custom_data->totlayer = kept_layers_data.size;
    custom_data->maxlayer = kept_layers_data.capacity;
  }

  r_storage.attributes_array = static_cast<Attribute **>(
      MEM_malloc_arrayN(attributes_to_move.size(), sizeof(Attribute *), __func__));
  r_storage.attributes_num = attributes_to_move.size();
  r_storage.attributes_capacity = attributes_to_move.size();

  for (const int i : attributes_to_move.index_range()) {
    AttributeToMove &src = attributes_to_move[i];
    Attribute *dst = MEM_cnew<Attribute>(__func__);
    r_storage.attributes_array[i] = dst;

    dst->name = BLI_strdupn(src.name.data(), src.name.size());
    dst->domain = int16_t(src.domain);
    dst->data_type = int16_t(src.attr_type);
    dst->storage_type = int8_t(AttrStorageType::Array);
    dst->data = src.data;
    dst->sharing_info = src.sharing_info;
  }
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
    AttributeStorage &storage, const std::array<CustomData *, ATTR_DOMAIN_NUM> custom_data_domains)
{
  for (Attribute *attribute : Span(storage.attributes_array, storage.attributes_num)) {
    if (const std::optional<eCustomDataType> data_type = attribute_to_to_custom_data_type(
            AttrType(attribute->data_type)))
    {
      CustomData_add_layer_named_with_data(custom_data_domains[attribute->domain],
                                           *data_type,
                                           attribute->data,
                                           0,  // TODO
                                           attribute->name,
                                           attribute->sharing_info);
    }
  }
}  // namespace blender::bke
