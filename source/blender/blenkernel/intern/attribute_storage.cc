/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_implicit_sharing.hh"
#include "BLI_string.h"

#include "DNA_attribute_types.h"

#include "BKE_attribute.hh"

using blender::StringRef;

namespace blender::bke {

struct AttributeStorageRuntime {
  Map<StringRef, std::reference_wrapper<Attribute>> name_map;
};

}  // namespace blender::bke

void Attribute::ensure_mutable()
{
  // TODO
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

void AttributeStorage::ensure_attribute_array_capacity(const int attributes_num)
{
  if (attributes_num > this->attributes_capacity) {
    this->attributes_capacity *= 2;
    this->attributes_array = static_cast<Attribute **>(
        MEM_reallocN(this->attributes_array, sizeof(Attribute) * attributes_num));
    this->attributes_capacity = attributes_num;
  }
}

Attribute &AttributeStorage::add(const StringRef name,
                                 const blender::bke::AttrDomain domain,
                                 const blender::bke::AttrType data_type,
                                 const blender::bke::AttrStorageType storage_type,
                                 const void *data,
                                 const blender::ImplicitSharingInfo *sharing_info)
{
  BLI_assert(!this->lookup(name));
  this->ensure_attribute_array_capacity(this->attributes_num + 1);
  Attribute &attribute = *this->attributes_array[this->attributes_num];
  this->attributes_num++;

  attribute.name = BLI_strdupn(name.data(), name.size());
  attribute.domain = int8_t(domain);
  attribute.data_type = int16_t(data_type);
  attribute.storage_type = int8_t(storage_type);
  attribute.data = data;
  attribute.sharing_info = sharing_info;
  attribute.sharing_info->add_user();
  this->runtime->name_map.add_new(StringRef(attribute.name, name.size()), attribute);
  return attribute;
}
