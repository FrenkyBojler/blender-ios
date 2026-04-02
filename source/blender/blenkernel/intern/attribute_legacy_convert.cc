/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define DNA_DEPRECATED_ALLOW

#include <optional>

#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_customdata.hh"

#include "BKE_attribute_legacy_convert.hh"

namespace blender::bke {

std::optional<AttrType> custom_data_type_to_attr_type(const eCustomDataType data_type)
{
  switch (data_type) {
    /* These types are not used for actual #CustomData layers. */
    case CD_NUMTYPES:
    case CD_AUTO_FROM_NAME:
    case CD_TANGENT:
      BLI_assert_unreachable();
      return std::nullopt;

    /* These types are only used for versioning old files. */
    case CD_MVERT:
    case CD_MSTICKY:
    case CD_MEDGE:
    case CD_FACEMAP:
    case CD_MTEXPOLY:
    case CD_MLOOPUV:
    case CD_MPOLY:
    case CD_MLOOP:
    case CD_BWEIGHT:
    case CD_CREASE:
    case CD_PAINT_MASK:
    case CD_CUSTOMLOOPNORMAL:
    case CD_SCULPT_FACE_SETS:
    case CD_MTFACE:
    case CD_TESSLOOPNORMAL:
    case CD_FREESTYLE_EDGE:
    case CD_FREESTYLE_FACE:
      return std::nullopt;

    /* These types are only used for #BMesh. */
    case CD_SHAPEKEY:
    case CD_SHAPE_KEYINDEX:
    case CD_BM_ELEM_PYPTR:
      return std::nullopt;

    /* Only used for legacy #MFace data. */
    case CD_MFACE:
    case CD_ORIGSPACE:
    case CD_MCOL:
      return std::nullopt;

    /* Custom data on vertices. */
    case CD_MDEFORMVERT:
    case CD_MVERT_SKIN:
    case CD_ORCO:
    case CD_CLOTH_ORCO:
      return std::nullopt;

    /* Custom data on face corners. */
    case CD_NORMAL:
    case CD_MDISPS:
    case CD_ORIGSPACE_MLOOP:
    case CD_GRID_PAINT_MASK:
      return std::nullopt;

    /* Use for editing/selecting original data from evaluated mesh (vertices, edges, faces). */
    case CD_ORIGINDEX:
      return std::nullopt;

    /* Used as a cache of tangents for current RNA API (face corners). */
    case CD_MLOOPTANGENT:
      return std::nullopt;

    /* Attribute types. */
    case CD_PROP_FLOAT:
      return AttrType::FLOAT;
    case CD_PROP_INT32:
      return AttrType::INT32;
    case CD_PROP_BYTE_COLOR:
      return AttrType::COLOR_BYTE;
    case CD_PROP_FLOAT4X4:
      return AttrType::FLOAT4X4;
    case CD_PROP_INT16_2D:
      return AttrType::INT16_2_D;
    case CD_PROP_INT8:
      return AttrType::INT8;
    case CD_PROP_INT32_2D:
      return AttrType::INT32_2_D;
    case CD_PROP_COLOR:
      return AttrType::COLOR_FLOAT;
    case CD_PROP_FLOAT3:
      return AttrType::FLOAT3;
    case CD_PROP_FLOAT4:
      return AttrType::FLOAT4;
    case CD_PROP_FLOAT2:
      return AttrType::FLOAT2;
    case CD_PROP_BOOL:
      return AttrType::BOOL;
    case CD_PROP_STRING:
      return AttrType::STRING;
    case CD_PROP_QUATERNION:
      return AttrType::QUATERNION;
  }
  return std::nullopt;
}

struct CustomDataAndSize {
  CustomData &data;
  int size;
};

/**
 * Move generic attributes from #CustomData to #AttributeStorage. All other non-generic layers are
 * left in #CustomData.
 */
static void attribute_legacy_convert_customdata_to_storage(
    const Map<AttrDomain, CustomDataAndSize> &domains, AttributeStorage &storage)
{
  struct AttributeToAdd {
    StringRef name;
    AttrDomain domain;
    AttrType type;
    void *array_data;
    int array_size;
    const ImplicitSharingInfo *sharing_info;
  };
  Map<AttrDomain, Vector<CustomDataLayer>> layers_to_keep;
  Vector<AttributeToAdd> attributes_to_add;
  for (const auto &item : domains.items()) {
    const AttrDomain domain = item.key;
    const CustomData &custom_data = item.value.data;
    const int domain_size = item.value.size;
    for (const CustomDataLayer &layer : MutableSpan(custom_data.layers, custom_data.totlayer)) {
      if (const std::optional<AttrType> attr_type = custom_data_type_to_attr_type(
              eCustomDataType(layer.type)))
      {
        /* Skip adding a user. This #CustomDataLayer is just freed below. */
        attributes_to_add.append(
            {layer.name, domain, *attr_type, layer.data, domain_size, layer.sharing_info});
      }
      else {
        layers_to_keep.lookup_or_add_default(domain).append(layer);
      }
    }
  }

  for (AttributeToAdd &attribute : attributes_to_add) {
    bke::Attribute::ArrayData array_data;
    array_data.data = attribute.array_data;
    array_data.size = attribute.array_size;
    array_data.sharing_info = ImplicitSharingPtr<>(attribute.sharing_info);
    if (Attribute *attr = storage.lookup(attribute.name)) {
      attr->assign_data(std::move(array_data));
    }
    else {
      storage.add(storage.unique_name_calc(attribute.name),
                  attribute.domain,
                  attribute.type,
                  std::move(array_data));
    }
  }

  for (const auto &[domain, custom_data] : domains.items()) {
    Vector layers_vector = layers_to_keep.pop_default(domain, {});
    MEM_SAFE_DELETE(custom_data.data.layers);
    custom_data.data.totlayer = 0;
    custom_data.data.maxlayer = 0;
    if (layers_vector.is_empty()) {
      CustomData_update_typemap(&custom_data.data);
      continue;
    }
    VectorData data = layers_vector.release();
    custom_data.data.layers = data.data;
    custom_data.data.totlayer = data.size;
    custom_data.data.maxlayer = data.capacity;
    CustomData_update_typemap(&custom_data.data);
  }
}

std::optional<eCustomDataType> attr_type_to_custom_data_type(const AttrType attr_type)
{
  switch (attr_type) {
    case AttrType::BOOL:
      return CD_PROP_BOOL;
    case AttrType::INT8:
      return CD_PROP_INT8;
    case AttrType::INT16_2_D:
      return CD_PROP_INT16_2D;
    case AttrType::INT32:
      return CD_PROP_INT32;
    case AttrType::INT32_2_D:
      return CD_PROP_INT32_2D;
    case AttrType::FLOAT:
      return CD_PROP_FLOAT;
    case AttrType::FLOAT2:
      return CD_PROP_FLOAT2;
    case AttrType::FLOAT3:
      return CD_PROP_FLOAT3;
    case AttrType::FLOAT4:
      return CD_PROP_FLOAT4;
    case AttrType::FLOAT4X4:
      return CD_PROP_FLOAT4X4;
    case AttrType::COLOR_BYTE:
      return CD_PROP_BYTE_COLOR;
    case AttrType::COLOR_FLOAT:
      return CD_PROP_COLOR;
    case AttrType::QUATERNION:
      return CD_PROP_QUATERNION;
    case AttrType::STRING:
      return CD_PROP_STRING;
  }
  return std::nullopt;
}

void mesh_convert_customdata_to_storage(Mesh &mesh)
{
  bke::attribute_legacy_convert_customdata_to_storage(
      {{AttrDomain::POINT, {mesh.vert_data, mesh.verts_num}},
       {AttrDomain::EDGE, {mesh.edge_data, mesh.edges_num}},
       {AttrDomain::FACE, {mesh.face_data, mesh.faces_num}},
       {AttrDomain::CORNER, {mesh.corner_data, mesh.corners_num}}},
      mesh.attribute_storage.wrap());
}

void curves_convert_customdata_to_storage(CurvesGeometry &curves)
{
  attribute_legacy_convert_customdata_to_storage(
      {{AttrDomain::POINT, {curves.point_data, curves.points_num()}},
       {AttrDomain::CURVE, {curves.curve_data_legacy, curves.curves_num()}}},
      curves.attribute_storage.wrap());
  CustomData_reset(&curves.curve_data_legacy);
  /* Update the curve type count again (the first time was done on file-read, where
   * #AttributeStorage data doesn't exist yet for older files). */
  curves.update_curve_types();
}

void pointcloud_convert_customdata_to_storage(PointCloud &pointcloud)
{
  attribute_legacy_convert_customdata_to_storage(
      {{AttrDomain::POINT, {pointcloud.pdata_legacy, pointcloud.totpoint}}},
      pointcloud.attribute_storage.wrap());
  CustomData_reset(&pointcloud.pdata_legacy);
}

void grease_pencil_convert_customdata_to_storage(GreasePencil &grease_pencil)
{
  attribute_legacy_convert_customdata_to_storage(
      {{AttrDomain::LAYER,
        {grease_pencil.layers_data_legacy, int(grease_pencil.layers().size())}}},
      grease_pencil.attribute_storage.wrap());
  CustomData_reset(&grease_pencil.layers_data_legacy);
}

static const CustomData &get_custom_data(const Mesh &mesh, const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::POINT:
      return mesh.vert_data;
    case AttrDomain::EDGE:
      return mesh.edge_data;
    case AttrDomain::FACE:
      return mesh.face_data;
    case AttrDomain::CORNER:
      return mesh.corner_data;
    default:
      BLI_assert_unreachable();
      return mesh.vert_data;
  }
}

static CustomData &get_custom_data(Mesh &mesh, const AttrDomain domain)
{
  return const_cast<CustomData &>(get_custom_data(std::as_const(mesh), domain));
}

static int get_domain_size(const Mesh &mesh, const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::POINT:
      return mesh.verts_num;
    case AttrDomain::EDGE:
      return mesh.edges_num;
    case AttrDomain::FACE:
      return mesh.faces_num;
    case AttrDomain::CORNER:
      return mesh.corners_num;
    default:
      BLI_assert_unreachable();
      return 0;
  }
}

LegacyMeshInterpolator::LegacyMeshInterpolator(const Mesh &src, Mesh &dst, const AttrDomain domain)
    : cd_src_(get_custom_data(src, domain)), cd_dst_(get_custom_data(dst, domain))
{
  const AttributeStorage &src_attributes = src.attribute_storage.wrap();
  AttributeStorage &dst_attributes = dst.attribute_storage.wrap();
  const int src_domain_size = get_domain_size(src, domain);
  const int dst_domain_size = get_domain_size(dst, domain);
  for (const Attribute &src_attr : src_attributes) {
    if (src_attr.domain() != domain) {
      continue;
    }
    Attribute *dst_attr = dst_attributes.lookup(src_attr.name());
    if (!dst_attr) {
      continue;
    }
    if (dst_attr->domain() != domain) {
      continue;
    }
    if (dst_attr->data_type() != src_attr.data_type()) {
      continue;
    }
    if (dst_attr->storage_type() != AttrStorageType::ARRAY) {
      continue;
    }
    const CPPType &cpp_type = attribute_type_to_cpp_type(src_attr.data_type());
    switch (src_attr.storage_type()) {
      case AttrStorageType::SINGLE: {
        const auto &value = std::get<Attribute::SingleData>(src_attr.data());
        attrs_src_.append(GVArray::from_single_ref(cpp_type, src_domain_size, value.value));
        break;
      }
      case AttrStorageType::ARRAY: {
        const auto &value = std::get<Attribute::ArrayData>(src_attr.data());
        attrs_src_.append(GVArray::from_span({cpp_type, value.data, src_domain_size}));
        break;
      }
    }
    auto &value = std::get<Attribute::ArrayData>(dst_attr->data_for_write());
    attrs_dst_.append({cpp_type, value.data, dst_domain_size});
  }
}

void LegacyMeshInterpolator::copy(const int src_index, const int dst_index, const int count) const
{
  if (count == 0) {
    return;
  }
  CustomData_copy_data(&cd_src_, &cd_dst_, src_index, dst_index, count);
  for (const int i : attrs_src_.index_range()) {
    const GVArraySpan &src = attrs_src_[i];
    GMutableSpan dst = attrs_dst_[i];
    src.type().copy_construct_compressed(src.data(), dst[dst_index], IndexRange(src_index, count));
  }
}

void LegacyMeshInterpolator::mix(Span<int> src_indices,
                                 const std::optional<Span<float>> weights,
                                 const int dst_index) const
{
  CustomData_interp(&cd_src_,
                    &cd_dst_,
                    src_indices.data(),
                    weights ? weights->data() : nullptr,
                    src_indices.size(),
                    dst_index);
  for (const int attr_index : attrs_src_.index_range()) {
    attribute_math::to_static_type(attrs_src_[attr_index].type(), [&]<typename T>() {
      if constexpr (!std::is_void_v<bke::attribute_math::DefaultMixer<T>>) {
        const Span src = attrs_src_[attr_index].typed<T>();
        MutableSpan dst = attrs_dst_[attr_index].typed<T>();
        if (weights) {
          dst[dst_index] = attribute_math::mix_indices(src, src_indices, *weights);
        }
        else {
          dst[dst_index] = attribute_math::mix_indices(src, src_indices);
        }
      }
    });
  }
}

}  // namespace blender::bke
