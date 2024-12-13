/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_attribute_types.h"

#include "BKE_attribute.hh"

namespace blender::bke {

struct AttributeStorageRuntime {
  Map<StringRef, std::reference_wrapper<Attribute>> name_map;
};

}  // namespace blender::bke

void Attribute::ensure_mutable()
{
  // TODO
}

const Attribute *AttributeStorage::lookup(blender::StringRef name) const
{
  std::reference_wrapper<Attribute> *attribute = this->runtime->name_map.lookup_ptr(name);
  if (!attribute) {
    return nullptr;
  }
  return &attribute->get();
}

Attribute *AttributeStorage::lookup_for_write(blender::StringRef name)
{
  std::reference_wrapper<Attribute> *attribute = this->runtime->name_map.lookup_ptr(name);
  if (!attribute) {
    return nullptr;
  }
  attribute->get().ensure_mutable();
  return &attribute->get();
}

bool AttributeStorage::remove(blender::StringRef name)
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

Attribute *AttributeStorage::add(blender::StringRef attribute_id,
                                 blender::bke::AttrDomain domain,
                                 blender::bke::AttrType data_type,
                                 blender::bke::AttrStorageType storage_type,
                                 const void *data,
                                 const blender::ImplicitSharingInfo *sharing_info)
{
}
