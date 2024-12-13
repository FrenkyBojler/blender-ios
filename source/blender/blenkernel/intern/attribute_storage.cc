/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_implicit_sharing.hh"
#include "BLI_string.h"

#include "DNA_attribute_types.h"

#include "BKE_attribute.hh"

using blender::CPPType;
using blender::ImplicitSharingInfo;
using blender::StringRef;
using blender::bke::AttrDomain;
using blender::bke::AttrStorageType;
using blender::bke::AttrType;

namespace blender::bke {

struct AttributeStorageRuntime {
  Map<StringRef, std::reference_wrapper<Attribute>> name_map;
};

}  // namespace blender::bke

void Attribute::ensure_mutable()
{
  using namespace blender::bke;
  switch (AttrStorageType(this->storage_type)) {
    case AttrStorageType::Array: {
      AttributeDataArray &data = *static_cast<AttributeDataArray *>(this->data);
      if (data.sharing_info->is_mutable()) {
        data.sharing_info->tag_ensured_mutable();
        return;
      }

      const CPPType &cpp_type = attribute_type_to_cpp_type(AttrType(this->data_type));
      void *new_data = MEM_mallocN_aligned(data.elements_num, cpp_type.alignment(), __func__);
      cpp_type.copy_construct_n(data.data, new_data, data.elements_num);
      data.data = new_data;

      data.sharing_info->remove_user_and_delete_if_last();
      data.sharing_info = make_implicit_sharing_info_for_layer(
          cpp_type, new_data, data.elements_num);
      break;
    }
    case AttrStorageType::Single:
      BLI_assert_unreachable();
      break;
  }
}

const Attribute *AttributeStorage::lookup(const StringRef name) const
{
  std::reference_wrapper<Attribute> *attribute = this->runtime->name_map.lookup_ptr(name);
  if (!attribute) {
    return nullptr;
  }
  return &attribute->get();
}

Attribute *AttributeStorage::lookup_for_write(const StringRef name)
{
  std::reference_wrapper<Attribute> *attribute = this->runtime->name_map.lookup_ptr(name);
  if (!attribute) {
    return nullptr;
  }
  attribute->get().ensure_mutable();
  return &attribute->get();
}

bool AttributeStorage::remove(const StringRef name)
{
  const Attribute *attribute = this->lookup(name);
  if (!attribute) {
    return false;
  }
  this->runtime->name_map.remove(name);
  std::remove(this->attributes_array, this->attributes_array + this->attributes_num, attribute);
  this->attributes_num--;
  return true;
}

Attribute &AttributeStorage::add(const StringRef name,
                                 const AttrDomain domain,
                                 const AttrType data_type,
                                 const AttributeDataArray &data)
{
  Attribute &attribute = this->add_without_data(name, domain, data_type, AttrStorageType::Array);

  data.sharing_info->add_user();
  AttributeDataArray *attribute_data = static_cast<AttributeDataArray *>(
      MEM_mallocN(sizeof(AttributeDataArray), __func__));
  memcpy(attribute_data, &data, sizeof(AttributeDataArray));
  attribute.data = attribute_data;
  return attribute;
}

void AttributeStorage::ensure_attribute_array_capacity(const int attributes_num)
{
  if (attributes_num > this->attributes_capacity) {
    this->attributes_capacity *= 2;
    this->attributes_array = static_cast<Attribute **>(
        MEM_reallocN(this->attributes_array, sizeof(Attribute) * attributes_num));
    this->attributes_capacity = attributes_num;
  }
}

Attribute &AttributeStorage::add_without_data(const StringRef name,
                                              const AttrDomain domain,
                                              const AttrType data_type,
                                              const AttrStorageType storage_type)
{
  BLI_assert(!this->lookup(name));
  this->ensure_attribute_array_capacity(this->attributes_num + 1);

  this->attributes_array[this->attributes_num] = static_cast<Attribute *>(
      MEM_mallocN(sizeof(Attribute), __func__));
  Attribute &attribute = *this->attributes_array[this->attributes_num];
  this->attributes_num++;

  attribute.name = BLI_strdupn(name.data(), name.size());
  attribute.domain = int8_t(domain);
  attribute.data_type = int16_t(data_type);
  attribute.storage_type = int8_t(storage_type);
  this->runtime->name_map.add_new(StringRef(attribute.name, name.size()), attribute);

  return attribute;
}
