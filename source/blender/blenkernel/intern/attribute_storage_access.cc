/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"
#include "BKE_deform.hh"
#include "BLI_listbase.h"
#include "DNA_object_types.h"

#include "attribute_storage_access.hh"

namespace blender::bke {

GAttributeReader attribute_to_reader(const Attribute &attribute,
                                     const AttrDomain domain,
                                     const int64_t domain_size)
{
  const CPPType &cpp_type = attribute_type_to_cpp_type(attribute.data_type());
  switch (attribute.storage_type()) {
    case AttrStorageType::Array: {
      const auto &data = std::get<Attribute::ArrayData>(attribute.data());
      return GAttributeReader{GVArray::from_span(GSpan(cpp_type, data.data, data.size)),
                              domain,
                              data.sharing_info.get()};
    }
    case AttrStorageType::Single: {
      const auto &data = std::get<Attribute::SingleData>(attribute.data());
      return GAttributeReader{GVArray::from_single_ref(cpp_type, domain_size, data.value),
                              domain,
                              data.sharing_info.get()};
    }
  }
  BLI_assert_unreachable();
  return {};
}

GAttributeWriter attribute_to_writer(void *owner,
                                     const Map<StringRef, AttrUpdateOnChange> &changed_tags,
                                     const int64_t domain_size,
                                     Attribute &attribute)
{
  const CPPType &cpp_type = attribute_type_to_cpp_type(attribute.data_type());
  switch (attribute.storage_type()) {
    case AttrStorageType::Array: {
      auto &data = std::get<Attribute::ArrayData>(attribute.data_for_write());
      BLI_assert(data.size == domain_size);

      std::function<void()> tag_modified_fn;
      if (const AttrUpdateOnChange update_fn = changed_tags.lookup_default(attribute.name(),
                                                                           nullptr))
      {
        tag_modified_fn = [owner, update_fn]() { update_fn(owner); };
      };

      return GAttributeWriter{
          GVMutableArray::from_span(GMutableSpan(cpp_type, data.data, domain_size)),
          attribute.domain(),
          std::move(tag_modified_fn)};
    }
    case AttrStorageType::Single: {
      /* Just convert the stored type to an array for modification. It might not make sense to
       * implement editing of single values at this level. */
      const auto &data = std::get<Attribute::SingleData>(attribute.data());
      const GPointer value(cpp_type, data.value);
      attribute.assign_data(Attribute::ArrayData::from_value(value, domain_size));
      return attribute_to_writer(owner, changed_tags, domain_size, attribute);
    }
  }
  BLI_assert_unreachable();
  return {};
}

Attribute::DataVariant attribute_init_to_data(const bke::AttrType data_type,
                                              const int64_t domain_size,
                                              const AttributeInit &initializer,
                                              const bool require_array_data)
{
  switch (initializer.type) {
    case AttributeInit::Type::Construct: {
      const CPPType &type = bke::attribute_type_to_cpp_type(data_type);
      return Attribute::ArrayData::from_constructed(type, domain_size);
    }
    case AttributeInit::Type::Value: {
      const auto &init = static_cast<const AttributeInitValue &>(initializer);
      BLI_assert(*init.value.type() == bke::attribute_type_to_cpp_type(data_type));
      if (require_array_data) {
        return Attribute::ArrayData::from_value(init.value, domain_size);
      }
      return Attribute::SingleData::from_value(init.value);
    }
    case AttributeInit::Type::DefaultValue: {
      const CPPType &type = bke::attribute_type_to_cpp_type(data_type);
      return Attribute::ArrayData::from_default_value(type, domain_size);
    }
    case AttributeInit::Type::VArray: {
      const auto &init = static_cast<const AttributeInitVArray &>(initializer);
      const GVArray &varray = init.varray;
      BLI_assert(varray.size() == domain_size);
      if (!require_array_data) {
        const CommonVArrayInfo &info = varray.common_info();
        if (info.type == CommonVArrayInfo::Type::Single) {
          return Attribute::SingleData::from_value(GPointer(varray.type(), info.data));
        }
      }
      const CPPType &type = varray.type();
      Attribute::ArrayData data = Attribute::ArrayData::from_uninitialized(type, domain_size);
      varray.materialize_to_uninitialized(varray.index_range(), data.data);
      return data;
    }
    case AttributeInit::Type::MoveArray: {
      const auto &init = static_cast<const AttributeInitMoveArray &>(initializer);
      Attribute::ArrayData data;
      data.data = init.data;
      data.size = domain_size;
      data.sharing_info = ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data.data));
      return data;
    }
    case AttributeInit::Type::Shared: {
      const auto &init = static_cast<const AttributeInitShared &>(initializer);
      Attribute::ArrayData data;
      data.data = const_cast<void *>(init.data);
      data.size = domain_size;
      data.sharing_info = ImplicitSharingPtr<>(init.sharing_info);
      data.sharing_info->add_user();
      return data;
    }
  }
  BLI_assert_unreachable();
  return {};
}

GVArray get_varray_attribute(const AttributeStorage &storage,
                             AttrDomain domain,
                             const CPPType &cpp_type,
                             StringRef name,
                             int64_t domain_size,
                             const void *default_value)
{
  const bke::Attribute *attr = storage.wrap().lookup(name);

  const auto return_default = [&]() {
    return GVArray::from_single(cpp_type, domain_size, default_value);
  };

  if (!attr) {
    return return_default();
  }
  if (attr->domain() != domain) {
    return return_default();
  }
  if (attr->data_type() != cpp_type_to_attribute_type(cpp_type)) {
    return return_default();
  }
  switch (attr->storage_type()) {
    case bke::AttrStorageType::Array: {
      const auto &data = std::get<bke::Attribute::ArrayData>(attr->data());
      const GSpan span(cpp_type, data.data, data.size);
      return GVArray::from_span(span);
    }
    case bke::AttrStorageType::Single: {
      const auto &data = std::get<bke::Attribute::SingleData>(attr->data());
      return GVArray::from_single(cpp_type, domain_size, data.value);
    }
  }
  return return_default();
}

std::optional<GSpan> get_span_attribute(const AttributeStorage &storage,
                                        const AttrDomain domain,
                                        const CPPType &cpp_type,
                                        const StringRef name,
                                        const int64_t domain_size)
{
  const bke::Attribute *attr = storage.wrap().lookup(name);
  if (!attr) {
    return {};
  }
  if (attr->domain() != domain) {
    return {};
  }
  if (const auto *array_data = std::get_if<bke::Attribute::ArrayData>(&attr->data())) {
    BLI_assert(array_data->size == domain_size);
    UNUSED_VARS_NDEBUG(domain_size);
    return GSpan(cpp_type, array_data->data, array_data->size);
  }
  return {};
}

GMutableSpan get_mutable_attribute(AttributeStorage &storage,
                                   const AttrDomain domain,
                                   const CPPType &cpp_type,
                                   const StringRef name,
                                   const int64_t domain_size,
                                   const void *custom_default_value)
{
  if (domain_size <= 0) {
    return {};
  }
  const bke::AttrType type = bke::cpp_type_to_attribute_type(cpp_type);
  if (bke::Attribute *attr = storage.wrap().lookup(name)) {
    if (attr->data_type() == type) {
      if (const auto *single_data = std::get_if<bke::Attribute::SingleData>(&attr->data())) {
        /* Convert single value storage to array storage. */
        const GPointer g_value(cpp_type, single_data->value);
        attr->assign_data(bke::Attribute::ArrayData::from_value(g_value, domain_size));
      }
      auto &array_data = std::get<bke::Attribute::ArrayData>(attr->data_for_write());
      return GMutableSpan(cpp_type, array_data.data, domain_size);
    }
    /* The attribute has the wrong type. This shouldn't happen for builtin attributes, but just
     * in case, remove it. */
    storage.wrap().remove(name);
  }
  const void *default_value = custom_default_value ? custom_default_value :
                                                     cpp_type.default_value();
  bke::Attribute &attr = storage.wrap().add(
      name,
      domain,
      type,
      bke::Attribute::ArrayData::from_value({cpp_type, default_value}, domain_size));
  auto &array_data = std::get<bke::Attribute::ArrayData>(attr.data_for_write());
  BLI_assert(array_data.size == domain_size);
  return GMutableSpan(cpp_type, array_data.data, domain_size);
}

bool try_delete_vertex_group(ListBaseT<bDeformGroup> &vertex_groups,
                             const StringRef name,
                             FunctionRef<MutableSpan<MDeformVert>()> get_mutable_dverts)
{
  int index;
  bDeformGroup *group;
  if (!BKE_defgroup_listbase_name_find(&vertex_groups, name, &index, &group)) {
    return false;
  }
  BLI_remlink(&vertex_groups, group);
  MEM_delete(group);
  MutableSpan<MDeformVert> dverts = get_mutable_dverts();
  if (dverts.is_empty()) {
    return true;
  }
  remove_defgroup_index(dverts, index);
  return true;
}

static bool try_renaming_vertex_group(ListBaseT<bDeformGroup> &vertex_groups,
                                      const StringRef old_name,
                                      const StringRef new_name,
                                      const bool overwrite,
                                      FunctionRef<MutableSpan<MDeformVert>()> get_mutable_dverts)
{
  if (overwrite) {
    try_delete_vertex_group(vertex_groups, new_name, get_mutable_dverts);
  }
  for (bDeformGroup &group : vertex_groups) {
    if (group.name == old_name) {
      new_name.copy_utf8_truncated(group.name);
      return true;
    }
  }
  return false;
}

Set<StringRef> rename_attributes(AttributeStorage &storage,
                                 const Map<StringRef, StringRef> &name_map,
                                 const bool overwrite,
                                 const Map<StringRef, AttrBuiltinInfo> &builtin_attributes,
                                 std::optional<ListBaseT<bDeformGroup> *> vertex_groups,
                                 FunctionRef<MutableSpan<MDeformVert>()> get_mutable_dverts)
{
  Set<StringRef> names_to_remove;
  Set<StringRef> failed;
  Map<Attribute *, StringRef> map;
  map.reserve(name_map.size());
  for (const auto &[old_name, new_name] : name_map.items()) {
    if (new_name.is_empty()) {
      failed.add_new(old_name);
      continue;
    }
    const AttrBuiltinInfo &old_builtin_info = builtin_attributes.lookup(old_name);
    const AttrBuiltinInfo &new_builtin_info = builtin_attributes.lookup(new_name);
    if (!old_builtin_info.deletable) {
      failed.add_new(old_name);
      continue;
    }
    Attribute *attr = storage.lookup(old_name);
    if (!attr) {
      failed.add_new(old_name);
      continue;
    }
    if (new_builtin_info.domain != attr->domain()) {
      failed.add_new(old_name);
      continue;
    }
    if (new_builtin_info.type != attr->data_type()) {
      failed.add_new(old_name);
      continue;
    }
    if (overwrite) {
      /* If we can replace existing attributes, make sure it's removed first. */
      names_to_remove.add_new(new_name);
    }
    else {
      /* If we can't replace existing attributes, skip this rename. */
      if (storage.lookup(new_name)) {
        failed.add_new(old_name);
        continue;
      }
    }
    map.add_new(attr, new_name);
  }

  if (vertex_groups) {
    for (const StringRef name : failed) {
      if (try_renaming_vertex_group(
              **vertex_groups, name, name_map.lookup(name), overwrite, get_mutable_dverts))
      {
        failed.remove_contained(name);
      }
    }
  }

  if (!names_to_remove.is_empty()) {
    storage.remove(names_to_remove);
  }
  storage.rename(map);
  return failed;
}

}  // namespace blender::bke
