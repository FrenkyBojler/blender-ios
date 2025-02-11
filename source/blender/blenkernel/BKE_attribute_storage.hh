/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_attribute_types.h"

struct BlendDataReader;
struct BlendWriter;

namespace blender::bke {

class Attribute {
  friend AttributeStorage;
  /** Attribute name. Cannot be changed without adding and removing attribute. */
  std::string name_;
  AttrDomain domain_;
  AttrType data_type_;

  struct ArrayData {
    void *data;
    int elements_num;
    const ImplicitSharingInfo *sharing_info;
    ~ArrayData();
  };
  struct SingleData {
    void *value;
    const ImplicitSharingInfo *sharing_info;
  };

 private:
  std::variant<ArrayData, SingleData> data_;

 public:
  Attribute() = default;
  Attribute(const Attribute &other);
  Attribute(Attribute &&other);
  Attribute &operator=(const Attribute &other);
  Attribute &operator=(Attribute &&other);
  ~Attribute();

  StringRefNull name() const;
  void ensure_mutable();
};

struct AttributeStorageRuntime {
  struct AttributeNameGetter {
    StringRef operator()(const std::unique_ptr<Attribute> &value) const
    {
      return value->name();
    }
  };
  CustomIDVectorSet<std::unique_ptr<Attribute>, AttributeNameGetter> attributes;
  auto items() const
  {
    return attributes.as_span();
  }
};

class AttributeStorage : public ::AttributeStorage {
 public:
  AttributeStorage();
  AttributeStorage(const AttributeStorage &other);
  AttributeStorage(AttributeStorage &&other);
  AttributeStorage &operator=(const AttributeStorage &other);
  AttributeStorage &operator=(AttributeStorage &&other);
  ~AttributeStorage();

  Span<std::unique_ptr<const Attribute>> items() const;
  Span<Attribute *> items();
  const Attribute *lookup(StringRef name) const;
  Attribute *lookup_for_write(StringRef name);
  bool remove(StringRef name);
  Attribute &add(StringRef name,
                 bke::AttrDomain domain,
                 bke::AttrType data_type,
                 const Attribute::ArrayData &data);

  void blend_read(BlendDataReader &reader);
  struct BlendWriteData {
    Vector<::Attribute *, 16> attribute_ptrs;
    Vector<::Attribute, 16> attibutes;
    Vector<::AttributeDataArray, 16> array_data;
  };
  void blend_write_prepare(BlendWriteData &write_data);
  void blend_write(BlendWriter &writer, const BlendWriteData &write_data) const;

 private:
  Attribute &add_without_data(StringRef name,
                              bke::AttrDomain domain,
                              bke::AttrType data_type,
                              bke::AttrStorageType storage_type);
};

inline Span<std::unique_ptr<const Attribute>> AttributeStorage::items() const
{
  return this->runtime->attributes.as_span();
}
inline Span<Attribute *> AttributeStorage::items()
{
  return this->runtime->attributes.as_span();
}

inline StringRefNull Attribute::name() const
{
  return name_;
};

}  // namespace blender::bke

inline blender::bke::AttributeStorage &AttributeStorage::wrap()
{
  return *reinterpret_cast<blender::bke::AttributeStorage *>(this);
}
inline const blender::bke::AttributeStorage &AttributeStorage::wrap() const
{
  return *reinterpret_cast<const blender::bke::AttributeStorage *>(this);
}
