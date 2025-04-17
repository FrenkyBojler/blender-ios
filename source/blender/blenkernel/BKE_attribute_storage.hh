/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BLI_function_ref.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_resource_scope.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_attribute_types.h"

struct BlendDataReader;
struct BlendWriter;

namespace blender::bke {

enum class AttrDomain : int8_t;
enum class AttrType : int16_t;
enum class AttrStorageType : int8_t;

/** Data and metadata for a single geometry attribute. */
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
  using DataVariant = std::variant<ArrayData, SingleData>;
  friend AttributeStorage;

 private:
  /**
   * Because it's used as the custom ID for the attributes vector set, the name cannot be changed
   * without adding and removing the attribute.
   */
  std::string name_;
  AttrDomain domain_;
  AttrType type_;
  AttrStorageType storage_type_;

  DataVariant data_;

 public:
  /**
   * Unique name across all domains.
   * \note Compared to #CustomData, which doesn't enforce uniqueness, across domains on its own,
   * this is enforced by asserts when adding attributes. See #unique_name_calc() (which is also
   * called during the conversion process).
   */
  StringRefNull name() const;

  /** Which part of a geometry the attribute corresponds to. */
  AttrDomain domain() const;

  /**
   * The data type exposed to the user. Depending on the storage type, the actual internal values
   * may not be the same type.
   */
  AttrType data_type() const;

  /**
   * The method used to store the data. This gives flexibility to optimize the internal storage
   * even though conceptually the attribute is an array of values.
   */
  AttrStorageType storage_type() const;

  /**
   * Low level access to the data stored for the attribute. The variant's type will correspond to
   * the storage type.
   */
  const DataVariant &data() const;

  /**
   * The same as #data(), but if the attribute data is shared initially, it will be unshared and
   * made mutable.
   */
  DataVariant &data_for_write();
};

class AttributeStorageRuntime {
  friend AttributeStorage;
  struct AttributeNameGetter {
    StringRef operator()(const std::unique_ptr<Attribute> &value) const
    {
      return value->name();
    }
  };
  /**
   * For quick access, the attributes are stored in a vector set, keyed by their name. Attributes
   * can still be reordered by rebuilding the vector set from scratch. Each attribute is allocated
   * to give pointer stability across additions and removals.
   */
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

  void foreach(FunctionRef<void(Attribute &)> fn);
  void foreach(FunctionRef<void(const Attribute &)> fn) const;
  Attribute *lookup(StringRef name);
  const Attribute *lookup(StringRef name) const;
  bool remove(StringRef name);
  Attribute &add(std::string name,
                 bke::AttrDomain domain,
                 bke::AttrType data_type,
                 Attribute::ArrayData data);
  std::string unique_name_calc(StringRef name);

  void blend_read(BlendDataReader &reader);
  struct BlendWriteData {
    ResourceScope scope;
    Vector<AttributeDNA, 16> attributes;
  };
  void blend_write(BlendWriter &writer, const BlendWriteData &write_data);

 private:
  Attribute &add_without_data(std::string name, bke::AttrDomain domain, bke::AttrType data_type);
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

inline const Attribute::DataVariant &Attribute::data() const
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
