/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_implicit_sharing.hh"
#include "BLI_string.h"

#include "BLO_read_write.hh"

#include "DNA_attribute_types.h"
#include "DNA_meshdata_types.h"

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

class ArrayDataImplicitSharing : public ImplicitSharingInfo {
 private:
  void *data_;
  int size_;
  const CPPType &type_;

 public:
  ArrayDataImplicitSharing(void *data, const int size, const CPPType &type)
      : ImplicitSharingInfo(), data_(data), size_(size), type_(type)
  {
  }

 private:
  void delete_self_with_data() override
  {
    if (data_ != nullptr) {
      type_.destruct_n(const_cast<void *>(data_), size_);
      MEM_freeN(const_cast<void *>(data_));
    }
    MEM_delete(this);
  }

  void delete_data_only() override
  {
    type_.destruct_n(const_cast<void *>(data_), size_);
    MEM_freeN(const_cast<void *>(data_));
    data_ = nullptr;
    size_ = 0;
  }
};

static ImplicitSharingInfo *create_sharing_info_for_array(const AttributeDataArray &data,
                                                          const CPPType &type)
{
  return MEM_new<ArrayDataImplicitSharing>(__func__, data.data, data.elements_num, type);
}

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
      data.sharing_info = create_sharing_info_for_array(data, cpp_type);
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
  Attribute **result = std::remove(
      this->attributes_array, this->attributes_array + this->attributes_num, attribute);
  BLI_assert(std::distance(this->attributes_array, result) == this->attributes_num - 1);
  this->attributes_num = std::distance(this->attributes_array, result);
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

static void read_attribute_data_array(BlendDataReader &reader,
                                      const AttrType data_type,
                                      AttributeDataArray &array_data)
{
  using namespace blender::bke;
  array_data.sharing_info = BLO_read_shared(
      &reader, &array_data.data, [&]() -> const ImplicitSharingInfo * {
        switch (data_type) {
          case AttrType::Bool:
            static_assert(sizeof(bool) == sizeof(int8_t));
            BLO_read_int8_array(&reader, array_data.elements_num, (int8_t **)(array_data.data));
            break;
          case AttrType::Int8:
            BLO_read_int8_array(&reader, array_data.elements_num, (int8_t **)(array_data.data));
            break;
          case AttrType::Int16_2D:
            BLO_read_int16_array(
                &reader, int64_t(array_data.elements_num) * 2, (int16_t **)(array_data.data));
            break;
          case AttrType::Int32:
            BLO_read_int32_array(&reader, array_data.elements_num, (int32_t **)(array_data.data));
            break;
          case AttrType::Int32_2D:
            BLO_read_int32_array(
                &reader, int64_t(array_data.elements_num) * 2, (int32_t **)(array_data.data));
            break;
          case AttrType::Float:
            BLO_read_float_array(&reader, array_data.elements_num, (float **)(array_data.data));
            break;
          case AttrType::Float2:
            BLO_read_float_array(
                &reader, int64_t(array_data.elements_num) * 2, (float **)(array_data.data));
            break;
          case AttrType::Float3:
            BLO_read_float3_array(&reader, array_data.elements_num, (float **)(array_data.data));
            ;
            break;
          case AttrType::Float4x4:
            BLO_read_float_array(
                &reader, int64_t(array_data.elements_num) * 16, (float **)(array_data.data));
            break;
          case AttrType::ColorByte:
            BLO_read_uint8_array(
                &reader, int64_t(array_data.elements_num) * 4, (uint8_t **)(array_data.data));
            break;
          case AttrType::ColorFloat:
            BLO_read_float_array(
                &reader, int64_t(array_data.elements_num) * 4, (float **)(array_data.data));
            break;
          case AttrType::Quaternion:
            BLO_read_float_array(
                &reader, int64_t(array_data.elements_num) * 4, (float **)(array_data.data));
            break;
          case AttrType::String:
            BLO_read_struct_array(&reader,
                                  MStringProperty,
                                  array_data.elements_num,
                                  (MStringProperty **)(array_data.data));
            break;
        }
        return create_sharing_info_for_array(array_data, attribute_type_to_cpp_type(data_type));
      });
}

void AttributeStorage::blend_read(BlendDataReader &reader)
{
  using namespace blender;
  BLO_read_pointer_array(&reader, this->attributes_num, (void **)(this->attributes_array));
  for (const int i : IndexRange(this->attributes_num)) {
    BLO_read_struct(&reader, Attribute, &this->attributes_array[i]);
    switch (AttrStorageType(this->attributes_array[i]->storage_type)) {
      case AttrStorageType::Array: {
        BLO_read_struct(&reader, AttributeDataArray, &this->attributes_array[i]->data);
        read_attribute_data_array(
            reader,
            AttrType(this->attributes_array[i]->data_type),
            *static_cast<AttributeDataArray *>(this->attributes_array[i]->data));
        break;
      }
      case AttrStorageType::Single: {
        BLI_assert_unreachable();
        break;
      }
    }
  }
}

static void write_attribute_data_array(BlendWriter &writer,
                                       const AttrType data_type,
                                       const AttributeDataArray &array_data)
{
  BLO_write_shared(
      &writer,
      array_data.data,
      attribute_type_to_cpp_type(data_type).size() * array_data.elements_num,
      array_data.sharing_info,
      [&]() {
        switch (data_type) {
          case AttrType::Bool:
            static_assert(sizeof(bool) == sizeof(int8_t));
            BLO_write_int8_array(
                &writer, array_data.elements_num, static_cast<const int8_t *>(array_data.data));
            break;
          case AttrType::Int8:
            BLO_write_int8_array(
                &writer, array_data.elements_num, static_cast<const int8_t *>(array_data.data));
            break;
          case AttrType::Int16_2D:
            BLO_write_int16_array(&writer,
                                  int64_t(array_data.elements_num) * 2,
                                  static_cast<const int16_t *>(array_data.data));
            break;
          case AttrType::Int32:
            BLO_write_int32_array(
                &writer, array_data.elements_num, static_cast<const int32_t *>(array_data.data));
            break;
          case AttrType::Int32_2D:
            BLO_write_int32_array(&writer,
                                  int64_t(array_data.elements_num) * 2,
                                  static_cast<const int32_t *>(array_data.data));
            break;
          case AttrType::Float:
            BLO_write_float_array(
                &writer, array_data.elements_num, static_cast<const float *>(array_data.data));
            break;
          case AttrType::Float2:
            BLO_write_float_array(&writer,
                                  int64_t(array_data.elements_num) * 2,
                                  static_cast<const float *>(array_data.data));
            break;
          case AttrType::Float3:
            BLO_write_float3_array(
                &writer, array_data.elements_num, static_cast<const float *>(array_data.data));
            break;
          case AttrType::Float4x4:
            BLO_write_float_array(&writer,
                                  int64_t(array_data.elements_num) * 16,
                                  static_cast<const float *>(array_data.data));
            break;
          case AttrType::ColorByte:
            BLO_write_uint8_array(&writer,
                                  int64_t(array_data.elements_num) * 4,
                                  static_cast<const uint8_t *>(array_data.data));
            break;
          case AttrType::ColorFloat:
            BLO_write_float_array(&writer,
                                  int64_t(array_data.elements_num) * 4,
                                  static_cast<const float *>(array_data.data));
            break;
          case AttrType::Quaternion:
            BLO_write_float_array(&writer,
                                  int64_t(array_data.elements_num) * 4,
                                  static_cast<const float *>(array_data.data));
            break;
          case AttrType::String:
            BLO_write_struct_array(&writer,
                                   MStringProperty,
                                   array_data.elements_num,
                                   static_cast<const MStringProperty *>(array_data.data));
            break;
        }
      });
}

void AttributeStorage::blend_write(BlendWriter &writer) const
{
  BLO_write_pointer_array(&writer, this->attributes_num, this->attributes_array);
  for (const Attribute *attribute : this->items()) {
    BLO_write_struct(&writer, Attribute, &attribute->data);
    switch (AttrStorageType(attribute->storage_type)) {
      case AttrStorageType::Array: {
        BLO_write_struct(&writer, AttributeDataArray, &attribute->data);
        write_attribute_data_array(writer,
                                   AttrType(attribute->data_type),
                                   *static_cast<const AttributeDataArray *>(attribute->data));
        break;
      }
      case AttrStorageType::Single: {
        BLI_assert_unreachable();
        break;
      }
    }
  }
}
