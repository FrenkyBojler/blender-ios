/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_implicit_sharing.hh"
#include "BLI_vector_set.hh"

#include "BLO_read_write.hh"

#include "DNA_attribute_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"

namespace blender::bke {

class ArrayDataImplicitSharing : public ImplicitSharingInfo {
 private:
  void *data_;
  int64_t size_;
  const CPPType &type_;

 public:
  ArrayDataImplicitSharing(void *data, const int64_t size, const CPPType &type)
      : ImplicitSharingInfo(), data_(data), size_(size), type_(type)
  {
  }

 private:
  void delete_self_with_data() override
  {
    if (data_ != nullptr) {
      type_.destruct_n(data_, size_);
      MEM_freeN(data_);
    }
    MEM_delete(this);
  }

  void delete_data_only() override
  {
    type_.destruct_n(data_, size_);
    MEM_freeN(data_);
    data_ = nullptr;
    size_ = 0;
  }
};

void AttributeStorage::foreach (FunctionRef<void(Attribute &)> fn)
{
  for (const std::unique_ptr<Attribute> &attribute : this->runtime->attributes) {
    fn(*attribute);
  }
}
void AttributeStorage::foreach (FunctionRef<void(const Attribute &)> fn) const
{
  for (const std::unique_ptr<Attribute> &attribute : this->runtime->attributes) {
    fn(*attribute);
  }
}

static ImplicitSharingInfo *create_sharing_info_for_array(void *data,
                                                          const int64_t elements_num,
                                                          const CPPType &type)
{
  return MEM_new<ArrayDataImplicitSharing>(__func__, data, elements_num, type);
}

std::variant<Attribute::ArrayData, Attribute::SingleData> &Attribute::data_for_write()
{
  if (auto *data = std::get_if<Attribute::ArrayData>(&data_)) {
    if (data->sharing_info->is_mutable()) {
      data->sharing_info->tag_ensured_mutable();
      return data_;
    }

    const CPPType &cpp_type = attribute_type_to_cpp_type(data_type_);
    void *new_data = MEM_malloc_arrayN_aligned(
        data->elements_num, cpp_type.size(), cpp_type.alignment(), __func__);
    cpp_type.copy_construct_n(data->data, new_data, data->elements_num);

    data->data = new_data;
    data->sharing_info = ImplicitSharingPtr<>(
        create_sharing_info_for_array(data->data, data->elements_num, cpp_type));
  }
  else if (std::get_if<Attribute::SingleData>(&data_)) {
    BLI_assert_unreachable();
  }
  return data_;
}

AttributeStorage::AttributeStorage()
{
  this->attributes_array = nullptr;
  this->attributes_num = 0;
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
}

AttributeStorage::AttributeStorage(const AttributeStorage &other)
{
  this->attributes_array = nullptr;
  this->attributes_num = 0;
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
  this->runtime->attributes.reserve(other.runtime->attributes.size());
  other.foreach ([&](const Attribute &attribute) {
    this->runtime->attributes.add_new(std::make_unique<Attribute>(attribute));
  });
}

AttributeStorage &AttributeStorage::operator=(const AttributeStorage &other)
{
  if (this == &other) {
    return *this;
  }
  std::destroy_at(this);
  new (this) AttributeStorage(other);
  return *this;
}

AttributeStorage::AttributeStorage(AttributeStorage &&other)
{
  this->attributes_array = nullptr;
  this->attributes_num = 0;
  this->runtime = other.runtime;
  other.runtime = nullptr;
}

AttributeStorage &AttributeStorage::operator=(AttributeStorage &&other)
{
  if (this == &other) {
    return *this;
  }
  std::destroy_at(this);
  new (this) AttributeStorage(std::move(other));
  return *this;
}

AttributeStorage::~AttributeStorage()
{
  /* These pointers are only used in files. */
  BLI_assert(this->attributes_array == nullptr);
  BLI_assert(this->attributes_num == 0);

  MEM_delete(this->runtime);
}

const Attribute *AttributeStorage::lookup(const StringRef name) const
{
  const std::unique_ptr<blender::bke::Attribute> *attribute =
      this->runtime->attributes.lookup_key_ptr_as(name);
  if (!attribute) {
    return nullptr;
  }
  return attribute->get();
}

Attribute *AttributeStorage::lookup(const StringRef name)
{
  const std::unique_ptr<blender::bke::Attribute> *attribute =
      this->runtime->attributes.lookup_key_ptr_as(name);
  if (!attribute) {
    return nullptr;
  }
  return attribute->get();
}

bool AttributeStorage::remove(const StringRef name)
{
  return this->runtime->attributes.remove_as(name);
}

Attribute &AttributeStorage::add(const StringRef name,
                                 const AttrDomain domain,
                                 const AttrType data_type,
                                 Attribute::ArrayData data)
{
  Attribute &attribute = this->add_without_data(name, domain, data_type);
  attribute.data_ = std::move(data);
  return attribute;
}

Attribute &AttributeStorage::add_without_data(const StringRef name,
                                              const AttrDomain domain,
                                              const AttrType data_type)
{
  BLI_assert(!this->lookup(name));
  std::unique_ptr<Attribute> ptr = std::make_unique<Attribute>();
  Attribute &attribute = *ptr;
  attribute.name_ = name;
  attribute.domain_ = domain;
  attribute.data_type_ = data_type;
  this->runtime->attributes.add_new(std::move(ptr));
  return attribute;
}

static void *read_attribute_data_array(BlendDataReader &reader,
                                       const AttrType data_type,
                                       const int64_t size,
                                       void **data,
                                       const ImplicitSharingInfo **sharing_info)
{
  const char *func = __func__;
  *sharing_info = BLO_read_shared(&reader, data, [&]() -> const ImplicitSharingInfo * {
    switch (data_type) {
      case AttrType::Bool:
        static_assert(sizeof(bool) == sizeof(int8_t));
        BLO_read_int8_array(&reader, size, (int8_t **)(data));
        break;
      case AttrType::Int8:
        BLO_read_int8_array(&reader, size, (int8_t **)(data));
        break;
      case AttrType::Int16_2D:
        BLO_read_int16_array(&reader, size * 2, (int16_t **)(data));
        break;
      case AttrType::Int32:
        BLO_read_int32_array(&reader, size, (int32_t **)(data));
        break;
      case AttrType::Int32_2D:
        BLO_read_int32_array(&reader, size * 2, (int32_t **)(data));
        break;
      case AttrType::Float:
        BLO_read_float_array(&reader, size, (float **)(data));
        break;
      case AttrType::Float2:
        BLO_read_float_array(&reader, size * 2, (float **)(data));
        break;
      case AttrType::Float3:
        BLO_read_float3_array(&reader, size, (float **)(data));
        break;
      case AttrType::Float4x4:
        BLO_read_float_array(&reader, size * 16, (float **)(data));
        break;
      case AttrType::ColorByte:
        BLO_read_uint8_array(&reader, size * 4, (uint8_t **)(data));
        break;
      case AttrType::ColorFloat:
        BLO_read_float_array(&reader, size * 4, (float **)(data));
        break;
      case AttrType::Quaternion:
        BLO_read_float_array(&reader, size * 4, (float **)(data));
        break;
      case AttrType::String:
        BLO_read_struct_array(&reader, MStringProperty, size, (MStringProperty **)(data));
        break;
    }
    const CPPType &cpp_type = attribute_type_to_cpp_type(data_type);
    return MEM_new<ArrayDataImplicitSharing>(func, data, size, cpp_type);
  });
}

void AttributeStorage::blend_read(BlendDataReader &reader)
{
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
  this->runtime->attributes.reserve(this->attributes_num);

  BLO_read_pointer_array(&reader, this->attributes_num, (void **)(&this->attributes_array));
  for (const int i : IndexRange(this->attributes_num)) {
    BLO_read_struct(&reader, AttributeDNA, &this->attributes_array[i]);
    AttributeDNA &dna_attr = *this->attributes_array[i];
    BLO_read_string(&reader, &dna_attr.name);

    std::unique_ptr<Attribute> attribute = std::make_unique<Attribute>();
    attribute->name_ = dna_attr.name;
    attribute->domain_ = AttrDomain(dna_attr.domain);
    attribute->data_type_ = AttrType(dna_attr.data_type);

    switch (AttrStorageType(dna_attr.storage_type)) {
      case AttrStorageType::Array: {
        BLO_read_struct(&reader, AttributeArrayDNA, &dna_attr.data);
        auto &data = *static_cast<AttributeArrayDNA *>(dna_attr.data);
        read_attribute_data_array(reader,
                                  AttrType(dna_attr.data_type),
                                  data.elements_num,
                                  &data.data,
                                  &data.sharing_info);
        attribute->data_ = Attribute::ArrayData{
            data.data, data.elements_num, ImplicitSharingPtr<>(data.sharing_info)};
        break;
      }
      case AttrStorageType::Single: {
        BLI_assert_unreachable();
        break;
      }
    }

    MEM_freeN(const_cast<char *>(dna_attr.name));
    MEM_freeN(dna_attr.data);
    MEM_freeN(&dna_attr);

    this->runtime->attributes.add_new(std::move(attribute));
  }

  /* These fields are not used at runtime. */
  MEM_SAFE_FREE(this->attributes_array);
  this->attributes_num = 0;
}

static void write_attribute_data_array(BlendWriter &writer,
                                       const AttrType data_type,
                                       const void *data,
                                       const int64_t size,
                                       const ImplicitSharingInfo &sharing_info)
{
  BLO_write_shared(
      &writer, data, attribute_type_to_cpp_type(data_type).size() * size, &sharing_info, [&]() {
        switch (data_type) {
          case AttrType::Bool:
            static_assert(sizeof(bool) == sizeof(int8_t));
            BLO_write_int8_array(&writer, size, static_cast<const int8_t *>(data));
            break;
          case AttrType::Int8:
            BLO_write_int8_array(&writer, size, static_cast<const int8_t *>(data));
            break;
          case AttrType::Int16_2D:
            BLO_write_int16_array(&writer, size * 2, static_cast<const int16_t *>(data));
            break;
          case AttrType::Int32:
            BLO_write_int32_array(&writer, size, static_cast<const int32_t *>(data));
            break;
          case AttrType::Int32_2D:
            BLO_write_int32_array(&writer, size * 2, static_cast<const int32_t *>(data));
            break;
          case AttrType::Float:
            BLO_write_float_array(&writer, size, static_cast<const float *>(data));
            break;
          case AttrType::Float2:
            BLO_write_float_array(&writer, size * 2, static_cast<const float *>(data));
            break;
          case AttrType::Float3:
            BLO_write_float3_array(&writer, size, static_cast<const float *>(data));
            break;
          case AttrType::Float4x4:
            BLO_write_float_array(&writer, size * 16, static_cast<const float *>(data));
            break;
          case AttrType::ColorByte:
            BLO_write_uint8_array(&writer, size * 4, static_cast<const uint8_t *>(data));
            break;
          case AttrType::ColorFloat:
            BLO_write_float_array(&writer, size * 4, static_cast<const float *>(data));
            break;
          case AttrType::Quaternion:
            BLO_write_float_array(&writer, size * 4, static_cast<const float *>(data));
            break;
          case AttrType::String:
            BLO_write_struct_array(
                &writer, MStringProperty, size, static_cast<const MStringProperty *>(data));
            break;
        }
      });
}

AttributeStorage::BlendWriteData AttributeStorage::blend_write_prepare()
{
  const Span<std::unique_ptr<Attribute>> attributes = this->runtime->attributes;
  BlendWriteData write_data;
  write_data.attribute_ptrs.reinitialize(attributes.size());
  write_data.attibutes.reinitialize(attributes.size());
  write_data.arrays.reserve(attributes.size());

  for (const int i : attributes.index_range()) {
    write_data.attribute_ptrs[i] = &write_data.attibutes[i];
    write_data.attibutes[i].name = attributes[i]->name().c_str();
    write_data.attibutes[i].domain = int8_t(attributes[i]->domain_);
    write_data.attibutes[i].data_type = int8_t(attributes[i]->data_type_);
    if (const auto *data = std::get_if<Attribute::ArrayData>(&attributes[i]->data_)) {
      write_data.attibutes[i].storage_type = int8_t(AttrStorageType::Array);
      write_data.arrays.append({});
      AttributeArrayDNA &dna_array = write_data.arrays.last();
      write_data.attibutes[i].data = &dna_array;
      dna_array.data = data->data;
      dna_array.elements_num = data->elements_num;
      dna_array.sharing_info = data->sharing_info.get();
    }
    else if (const auto *data = std::get_if<Attribute::SingleData>(&attributes[i]->data_)) {
      write_data.attibutes[i].storage_type = int8_t(AttrStorageType::Single);
      write_data.arrays.append({});
      AttributeArrayDNA &dna_array = write_data.arrays.last();
      write_data.attibutes[i].data = &dna_array;
      dna_array.data = data->value;
      dna_array.elements_num = 1;
      dna_array.sharing_info = data->sharing_info.get();
    }
  }

  this->attributes_array = write_data.attribute_ptrs.data();
  this->attributes_num = attributes.size();
  return write_data;
}

void AttributeStorage::blend_write(BlendWriter &writer,
                                   const AttributeStorage::BlendWriteData &write_data)
{
  BLO_write_pointer_array(
      &writer, write_data.attribute_ptrs.size(), write_data.attribute_ptrs.data());
  BLO_write_struct_array(
      &writer, AttributeDNA, write_data.attibutes.size(), write_data.attibutes.data());
  for (const AttributeDNA &attribute : write_data.attibutes) {
    BLO_write_struct(&writer, AttributeDNA, attribute);
    BLO_write_string(&writer, attribute.name);
  }
  BLO_write_struct_array(
      &writer, AttributeArrayDNA, write_data.array_data.size(), write_data.array_data.data());
  for (const AttributeArrayDNA &array_data : write_data.array_data) {
    write_attribute_data_array(
        writer, AttrType(array_data.data), array_data.data, array_data.sharing_info);
  }

  this->attributes_array = nullptr;
  this->attributes_num = 0;
}

}  // namespace blender::bke
