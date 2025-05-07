/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_pointcloud_types.h"

#include "BKE_attribute_legacy_convert.hh"
#include "BKE_pointcloud.hh"

#include "attribute_access_intern.hh"

namespace blender::bke {

static void tag_position_changed(void *owner)
{
  PointCloud &points = *static_cast<PointCloud *>(owner);
  points.tag_positions_changed();
}

static void tag_radius_changed(void *owner)
{
  PointCloud &points = *static_cast<PointCloud *>(owner);
  points.tag_radii_changed();
}

using UpdateOnChange = void (*)(void *owner);

static const auto &changed_tags()
{
  static Map<StringRef, UpdateOnChange> attributes{{"position", tag_position_changed},
                                                   {"radius", tag_radius_changed}};
  return attributes;
}

struct BuiltinInfo {
  bke::AttrDomain domain;
  bke::AttrType type;
  GPointer default_value;
  AttributeValidator validator;
  UpdateOnChange update_on_change;
  bool deletable;
};

static const auto &builtin_attributes()
{
  static Map<StringRef, BuiltinInfo> attributes;
  return attributes;
}

static GAttributeReader attribute_to_reader(const Attribute &attribute,
                                            const AttrDomain domain,
                                            const int64_t domain_size)
{
  const CPPType &cpp_type = attribute_type_to_cpp_type(attribute.data_type());
  const Attribute::DataVariant &data = attribute.data();
  if (const auto *array = std::get_if<Attribute::ArrayData>(&data)) {
    BLI_assert(domain_size == array->size);
    const GVArray varray = GVArray::ForSpan(GSpan(cpp_type, array->data, array->size));
    return GAttributeReader{varray, domain, array->sharing_info.get()};
  }
  if (const auto *single = std::get_if<Attribute::SingleData>(&data)) {
    const GVArray varray = GVArray::ForSingleRef(cpp_type, domain_size, single->value);
    return GAttributeReader{varray, domain, single->sharing_info.get()};
  }
  BLI_assert_unreachable();
  return {};
}

static GAttributeWriter attribute_to_writer(const StringRef name,Attribute &attribute,
                                            const AttrDomain domain,
                                            const int64_t domain_size)
{
  const CPPType &cpp_type = attribute_type_to_cpp_type(attribute.data_type());
  Attribute::DataVariant &data = attribute.data_for_write();
  if (auto *array = std::get_if<Attribute::ArrayData>(&data)) {
    BLI_assert(domain_size == array->size);
    return GAttributeWriter
    {
      GVMutableArray::ForSpan(GMutableSpan(cpp_type, array->data, array->size)), domain,
          changed_tags().lookup(name);
    };
  }
  if (auto *single = std::get_if<Attribute::SingleData>(&data)) {
    const GVArray varray = GVArray::ForSingleRef(cpp_type, domain_size, single->value);
    // return GAttributeWriter{varray, domain, single->sharing_info.get()};
  }
  BLI_assert_unreachable();
  return {};
}

static Attribute::DataVariant attribute_init_to_data(const AttributeInit &init)
{
  switch (init.type) {
    case AttributeInit::Type::Construct: {
      add_generic_custom_data_layer(
          custom_data, data_type, CD_CONSTRUCT, domain_num, attribute_id);
      break;
    }
    case AttributeInit::Type::DefaultValue: {
      if (const void *default_value = custom_default_value_ptr.get()) {
        const CPPType &type = *custom_default_value_ptr.type();
        void *data = add_generic_custom_data_layer(
            custom_data, data_type, CD_CONSTRUCT, domain_num, attribute_id);
        type.fill_assign_n(default_value, data, domain_num);
      }
      else {
        add_generic_custom_data_layer(
            custom_data, data_type, CD_SET_DEFAULT, domain_num, attribute_id);
      }
      break;
    }
    case AttributeInit::Type::VArray: {
      void *data = add_generic_custom_data_layer(
          custom_data, data_type, CD_CONSTRUCT, domain_num, attribute_id);
      if (data != nullptr) {
        const GVArray &varray = static_cast<const AttributeInitVArray &>(initializer).varray;
        varray.materialize_to_uninitialized(varray.index_range(), data);
      }
      break;
    }
    case AttributeInit::Type::MoveArray: {
      void *data = static_cast<const AttributeInitMoveArray &>(initializer).data;
      add_generic_custom_data_layer_with_existing_data(
          custom_data, data_type, attribute_id, domain_num, data, nullptr);
      break;
    }
    case AttributeInit::Type::Shared: {
      const AttributeInitShared &init = static_cast<const AttributeInitShared &>(initializer);
      add_generic_custom_data_layer_with_existing_data(custom_data,
                                                       data_type,
                                                       attribute_id,
                                                       domain_num,
                                                       const_cast<void *>(init.data),
                                                       init.sharing_info);
      break;
    }
  }
}

/**
 * In this function all the attribute providers for a point cloud component are created. Most data
 * in this function is statically allocated, because it does not change over time.
 */
static GeometryAttributeProviders create_attribute_providers_for_pointcloud()
{
  static CustomDataAccessInfo point_access = {
      [](void *owner) -> CustomData * {
        PointCloud *pointcloud = static_cast<PointCloud *>(owner);
        return &pointcloud->pdata;
      },
      [](const void *owner) -> const CustomData * {
        const PointCloud *pointcloud = static_cast<const PointCloud *>(owner);
        return &pointcloud->pdata;
      },
      [](const void *owner) -> int {
        const PointCloud *pointcloud = static_cast<const PointCloud *>(owner);
        return pointcloud->totpoint;
      }};

  static BuiltinCustomDataLayerProvider position("position",
                                                 AttrDomain::Point,
                                                 CD_PROP_FLOAT3,
                                                 BuiltinAttributeProvider::NonDeletable,
                                                 point_access,
                                                 tag_position_changed);
  static BuiltinCustomDataLayerProvider radius("radius",
                                               AttrDomain::Point,
                                               CD_PROP_FLOAT,
                                               BuiltinAttributeProvider::Deletable,
                                               point_access,
                                               tag_radius_changed);
  static BuiltinCustomDataLayerProvider id("id",
                                           AttrDomain::Point,
                                           CD_PROP_INT32,
                                           BuiltinAttributeProvider::Deletable,
                                           point_access,
                                           nullptr);
  static CustomDataAttributeProvider point_custom_data(AttrDomain::Point, point_access);
  return GeometryAttributeProviders({&position, &radius, &id}, {&point_custom_data});
}

static AttributeAccessorFunctions get_pointcloud_accessor_functions()
{
  static const GeometryAttributeProviders providers = create_attribute_providers_for_pointcloud();
  AttributeAccessorFunctions fn{};
  fn.domain_supported = [](const void * /*owner*/, const AttrDomain domain) {
    return domain == AttrDomain::Point;
  };
  fn.domain_size = [](const void *owner, const AttrDomain domain) {
    return domain == AttrDomain::Point ? static_cast<const PointCloud *>(owner)->totpoint : 0;
  };
  fn.builtin_domain_and_type = [](const void * /*owner*/,
                                  const StringRef name) -> std::optional<AttributeDomainAndType> {
    const BuiltinInfo *info = builtin_attributes().lookup_ptr(name);
    if (!info) {
      return std::nullopt;
    }
    const std::optional<eCustomDataType> cd_type = attribute_to_to_custom_data_type(info->type);
    BLI_assert(cd_type.has_value());
    return AttributeDomainAndType{info->domain, *cd_type};
  };
  fn.lookup = [](const void *owner, const StringRef name) -> GAttributeReader {
    const PointCloud &pointcloud = *static_cast<const PointCloud *>(owner);
    const AttributeStorage &storage = pointcloud.attribute_storage.wrap();
    const Attribute *attribute = storage.lookup(name);
    if (!attribute) {
      return {};
    }
    return attribute_to_reader(*attribute, AttrDomain::Point, pointcloud.totpoint);
  };
  fn.adapt_domain = [](const void * /*owner*/,
                       const GVArray &varray,
                       const AttrDomain from_domain,
                       const AttrDomain to_domain) {
    if (from_domain == to_domain && from_domain == AttrDomain::Point) {
      return varray;
    }
    return GVArray{};
  };
  fn.foreach_attribute = [](const void *owner,
                            const FunctionRef<void(const AttributeIter &)> fn,
                            const AttributeAccessor &accessor) {
    const PointCloud &pointcloud = *static_cast<const PointCloud *>(owner);
    const AttributeStorage &storage = pointcloud.attribute_storage.wrap();
    storage.foreach([&](const Attribute &attribute) {
      const auto get_fn = [&]() {
        return attribute_to_reader(attribute, AttrDomain::Point, pointcloud.totpoint);
      };
      const std::optional<eCustomDataType> cd_type = attribute_to_to_custom_data_type(
          attribute.data_type());
      BLI_assert(cd_type.has_value());
      AttributeIter iter(attribute.name(), attribute.domain(), *cd_type, get_fn);
      iter.is_builtin = builtin_attributes().contains(attribute.name());
      iter.accessor = &accessor;
      fn(iter);
      return !iter.is_stopped();
    });
  };
  fn.lookup_validator = [](const void * /*owner*/, const StringRef name) -> AttributeValidator {
    const BuiltinInfo *info = builtin_attributes().lookup_ptr(name);
    if (!info) {
      return {};
    }
    return info->validator;
  };
  fn.lookup_for_write = [](void *owner, const StringRef name) -> GAttributeWriter {
    PointCloud &pointcloud = *static_cast<PointCloud *>(owner);
    AttributeStorage &storage = pointcloud.attribute_storage.wrap();
    Attribute *attribute = storage.lookup(name);
    if (!attribute) {
      return {};
    }
    return attribute_to_writer(name,*attribute, AttrDomain::Point, pointcloud.totpoint);
  };
  fn.remove = [](void *owner, const StringRef name) -> bool {
    PointCloud &pointcloud = *static_cast<PointCloud *>(owner);
    AttributeStorage &storage = pointcloud.attribute_storage.wrap();
    const bool removed = storage.remove(name);
    if (removed) {
      if (const std::optional<UpdateOnChange> fn = changed_tags().lookup_try(name)) {
        (*fn)(owner);
      }
    }
    return removed;
  };
  fn.add = [](void *owner,
              const StringRef name,
              const AttrDomain domain,
              const eCustomDataType data_type,
              const AttributeInit &initializer) {
    PointCloud &pointcloud = *static_cast<PointCloud *>(owner);
    AttributeStorage &storage = pointcloud.attribute_storage.wrap();
    if (storage.lookup(name)) {
      return false;
    }
    const std::optional<AttrType> type = custom_data_type_to_attribute_type(data_type);
    Attribute::DataVariant data = attribute_init_to_data(initializer);
    storage.add(name, domain, type, std::move(data  ));
    return false;
  };

  return fn;
}

const AttributeAccessorFunctions &pointcloud_attribute_accessor_functions()
{
  static const AttributeAccessorFunctions fn = get_pointcloud_accessor_functions();
  return fn;
}

}  // namespace blender::bke
