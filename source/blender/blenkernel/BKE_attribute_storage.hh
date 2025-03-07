/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BLI_function_ref.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_attribute_types.h"

struct BlendDataReader;
struct BlendWriter;

namespace blender::bke {

class Attribute {
 public:
  struct ArrayData {
    void *data;
    int64_t size;
    ImplicitSharingPtr<> sharing_info;
  };
  struct SingleData {
    void *value;
    ImplicitSharingPtr<> sharing_info;
  };
  friend AttributeStorage;

 private:
  /** The name be changed without adding and removing attribute. */
  std::string name_;
  AttrDomain domain_;
  AttrType type_;
  AttrStorageType storage_type_;

  std::variant<ArrayData, SingleData> data_;

 public:
  StringRefNull name() const;
  AttrDomain domain() const;
  AttrStorageType storage_type() const;
  AttrType data_type() const;

  const std::variant<ArrayData, SingleData> &data() const;
  std::variant<ArrayData, SingleData> &data_for_write();
};

class AttributeStorageRuntime {
  friend AttributeStorage;
  struct AttributeNameGetter {
    StringRef operator()(const std::unique_ptr<Attribute> &value) const
    {
      return value->name();
    }
  };
  CustomIDVectorSet<std::unique_ptr<Attribute>, AttributeNameGetter> attributes;
};

class AttributeStorage : public ::AttributeStorage {
 public:
  AttributeStorage();
  AttributeStorage(const AttributeStorage &other);
  AttributeStorage(AttributeStorage &&other);
  AttributeStorage &operator=(const AttributeStorage &other);
  AttributeStorage &operator=(AttributeStorage &&other);
  ~AttributeStorage();

  void foreach (FunctionRef<void(Attribute &)> fn);
  void foreach (FunctionRef<void(const Attribute &)> fn) const;
  Attribute *lookup(StringRef name);
  const Attribute *lookup(StringRef name) const;
  bool remove(StringRef name);
  Attribute &add(StringRef name,
                 bke::AttrDomain domain,
                 bke::AttrType data_type,
                 Attribute::ArrayData data);

  void blend_read(BlendDataReader &reader);
  struct BlendWriteData {
    Array<AttributeDNA, 16> attibutes;
    Array<AttributeArrayDNA, 16> arrays;
    Array<AttributeSingleDNA, 16> singles;
  };
  void blend_write_prepare(BlendWriter &writer, AttributeStorage::BlendWriteData &write_data);
  void blend_write(BlendWriter &writer, const BlendWriteData &write_data);

 private:
  Attribute &add_without_data(StringRef name, bke::AttrDomain domain, bke::AttrType data_type);
};

inline StringRefNull Attribute::name() const
{
  return name_;
}

inline AttrDomain Attribute::domain() const
{
  return domain_;
}

inline AttrStorageType Attribute::storage_type() const
{
  return storage_type_;
}

inline AttrType Attribute::data_type() const
{
  return type_;
}

inline const std::variant<Attribute::ArrayData, Attribute::SingleData> &Attribute::data() const
{
  return data_;
}

}  // namespace blender::bke

inline blender::bke::AttributeStorage &AttributeStorage::wrap()
{
  return *reinterpret_cast<blender::bke::AttributeStorage *>(this);
}
inline const blender::bke::AttributeStorage &AttributeStorage::wrap() const
{
  return *reinterpret_cast<const blender::bke::AttributeStorage *>(this);
}
