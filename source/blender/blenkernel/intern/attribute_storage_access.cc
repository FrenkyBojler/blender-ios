/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"

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
      return GAttributeReader{GVArray::ForSpan(GSpan(cpp_type, data.data, data.size)),
                              domain,
                              data.sharing_info.get()};
    }
    case AttrStorageType::Single: {
      const auto &data = std::get<Attribute::SingleData>(attribute.data());
      return GAttributeReader{GVArray::ForSingleRef(cpp_type, domain_size, data.value),
                              domain,
                              data.sharing_info.get()};
    }
  }
  BLI_assert_unreachable();
  return {};
}

GAttributeWriter attribute_to_writer(void *owner,
                                     const Map<StringRef, UpdateOnChange> &changed_tags,
                                     const int64_t domain_size,
                                     Attribute &attribute)
{
  const CPPType &cpp_type = attribute_type_to_cpp_type(attribute.data_type());
  switch (attribute.storage_type()) {
    case AttrStorageType::Array: {
      auto &data = std::get<Attribute::ArrayData>(attribute.data_for_write());
      BLI_assert(data.size == domain_size);

      std::function<void()> tag_modified_fn;
      if (const UpdateOnChange update_fn = changed_tags.lookup_default(attribute.name(), nullptr))
      {
        tag_modified_fn = [owner, update_fn]() { update_fn(owner); };
      };

      return GAttributeWriter{
          GVMutableArray::ForSpan(GMutableSpan(cpp_type, data.data, domain_size)),
          attribute.domain(),
          std::move(tag_modified_fn)};
    }
    case AttrStorageType::Single: {
      /* Not yet implemented. */
      BLI_assert_unreachable();
    }
  }
  BLI_assert_unreachable();
  return {};
}

Attribute::DataVariant attribute_init_to_data(const bke::AttrType data_type,
                                              const int64_t domain_size,
                                              const AttributeInit &initializer)
{
  switch (initializer.type) {
    case AttributeInit::Type::Construct: {
      const CPPType &type = bke::attribute_type_to_cpp_type(data_type);
      return Attribute::ArrayData::ForConstructed(type, domain_size);
    }
    case AttributeInit::Type::DefaultValue: {
      const CPPType &type = bke::attribute_type_to_cpp_type(data_type);
      return Attribute::ArrayData::ForDefaultValue(type, domain_size);
    }
    case AttributeInit::Type::VArray: {
      const auto &init = static_cast<const AttributeInitVArray &>(initializer);
      const GVArray &varray = init.varray;
      BLI_assert(varray.size() == domain_size);
      const CPPType &type = varray.type();
      Attribute::ArrayData data;
      data.data = MEM_malloc_arrayN_aligned(domain_size, type.size, type.alignment, __func__);
      varray.materialize_to_uninitialized(varray.index_range(), data.data);
      data.size = domain_size;
      data.sharing_info = ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data.data));
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

}  // namespace blender::bke
