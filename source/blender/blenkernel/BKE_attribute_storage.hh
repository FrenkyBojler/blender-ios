/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_attribute_types.h"

struct BlendDataReader;
struct BlendWriter;

namespace blender::bke {

class Attribute : public ::Attribute {
 public:
  void ensure_mutable();
};

/**
 * \todo Move to .cc file when attribute_legacy_convert.cc no longer needs to remove attributes.
 */
struct AttributeStorageRuntime {
  struct AttributeNameGetter {
    StringRef operator()(const Attribute &value) const
    {
      return StringRef(value.name);
    }
  };
  CustomIDVectorSet<std::reference_wrapper<Attribute>, AttributeNameGetter> name_map;
};

class AttributeStorage : public ::AttributeStorage {
 public:
  AttributeStorage();
  AttributeStorage(const AttributeStorage &other);
  AttributeStorage(AttributeStorage &&other);
  AttributeStorage &operator=(const AttributeStorage &other);
  AttributeStorage &operator=(AttributeStorage &&other);
  ~AttributeStorage();

  Span<const Attribute *> items() const;
  MutableSpan<Attribute *> items();
  const Attribute *lookup(StringRef name) const;
  Attribute *lookup_for_write(StringRef name);
  bool remove(StringRef name);
  Attribute &add(StringRef name,
                 bke::AttrDomain domain,
                 bke::AttrType data_type,
                 const AttributeDataArray &data);

  void blend_read(BlendDataReader &reader);
  void blend_write(BlendWriter &writer) const;

 private:
  void ensure_attribute_array_capacity(int attributes_num);
  Attribute &add_without_data(StringRef name,
                              bke::AttrDomain domain,
                              bke::AttrType data_type,
                              bke::AttrStorageType storage_type);
};

inline Span<const Attribute *> AttributeStorage::items() const
{
  return Span(reinterpret_cast<Attribute **>(this->attributes_array), this->attributes_num);
}
inline MutableSpan<Attribute *> AttributeStorage::items()
{
  return MutableSpan(reinterpret_cast<Attribute **>(this->attributes_array), this->attributes_num);
}

}  // namespace blender::bke

inline blender::bke::Attribute &Attribute::wrap()
{
  return *reinterpret_cast<blender::bke::Attribute *>(this);
}
inline const blender::bke::Attribute &Attribute::wrap() const
{
  return *reinterpret_cast<const blender::bke::Attribute *>(this);
}

inline blender::bke::AttributeStorage &AttributeStorage::wrap()
{
  return *reinterpret_cast<blender::bke::AttributeStorage *>(this);
}
inline const blender::bke::AttributeStorage &AttributeStorage::wrap() const
{
  return *reinterpret_cast<const blender::bke::AttributeStorage *>(this);
}
