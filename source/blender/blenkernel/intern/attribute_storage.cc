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

void Attribute::ensure_mutable()
{
  if (auto *data = std::get_if<Attribute::ArrayData>(&data_)) {
    if (data->sharing_info->is_mutable()) {
      data->sharing_info->tag_ensured_mutable();
      return;
    }

    const CPPType &cpp_type = attribute_type_to_cpp_type(data_type_);
    void *new_data = MEM_mallocN_aligned(data->elements_num, cpp_type.alignment(), __func__);
    cpp_type.copy_construct_n(data->data, new_data, data->elements_num);
    data->data = new_data;

    data->sharing_info->remove_user_and_delete_if_last();
    data->sharing_info = create_sharing_info_for_array(data->data, data->elements_num, cpp_type);
  }
  else if (std::get_if<Attribute::SingleData>(&data_)) {
    BLI_assert_unreachable();
  }
}

AttributeStorage::AttributeStorage()
{
  this->attributes_array = nullptr;
  this->attributes_num = 0;
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
}

AttributeStorage::AttributeStorage(const AttributeStorage &other)
{
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
  this->attributes_array = other.attributes_array;
  other.attributes_array = nullptr;

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
  const Attribute *attribute = this->lookup(name);
  if (!attribute) {
    return false;
  }
  this->runtime->attributes.remove_as(name);
  ::Attribute **result = std::remove(
      this->attributes_array, this->attributes_array + this->attributes_num, attribute);
  BLI_assert(std::distance(this->attributes_array, result) == this->attributes_num - 1);
  this->attributes_num = std::distance(this->attributes_array, result);
  return true;
}

Attribute &AttributeStorage::add(const StringRef name,
                                 const AttrDomain domain,
                                 const AttrType data_type,
                                 const Attribute::ArrayData &data)
{
  Attribute &attribute = this->add_without_data(name, domain, data_type);
  data.sharing_info->add_user();
  attribute.data_ = data;
  return attribute;
}

Attribute &AttributeStorage::add_without_data(const StringRef name,
                                              const AttrDomain domain,
                                              const AttrType data_type)
{
  BLI_assert(!this->lookup_as(name));
  std::unique_ptr<Attribute> ptr = std::make_unique<Attribute>();
  Attribute &attribute = *ptr;
  attribute.name_ = name;
  attribute.domain_ = domain;
  attribute.data_type_ = data_type;
  this->runtime->attributes.add_new(std::move(ptr));
  return attribute;
}

static void read_attribute_data_array(BlendDataReader &reader,
                                      const AttrType data_type,
                                      AttributeDataArray &array_data)
{
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
        return MEM_new<ArrayDataImplicitSharing>("ArrayDataImplicitSharing",
                                                 array_data.data,
                                                 array_data.elements_num,
                                                 attribute_type_to_cpp_type(data_type));
      });
}

void AttributeStorage::blend_read(BlendDataReader &reader)
{
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
  this->runtime->attributes.reserve(this->attributes_num);

  BLO_read_pointer_array(&reader, this->attributes_num, (void **)(&this->attributes_array));
  for (const int i : IndexRange(this->attributes_num)) {
    BLO_read_struct(&reader, ::Attribute, &this->attributes_array[i]);
    ::Attribute &dna_attr = *this->attributes_array[i];
    BLO_read_string(&reader, &dna_attr.name);

    std::unique_ptr<Attribute> attribute = std::make_unique<Attribute>();
    attribute->name_ = dna_attr.name;
    attribute->domain_ = AttrDomain(dna_attr.domain);
    attribute->data_type_ = AttrType(dna_attr.data_type);

    switch (AttrStorageType(dna_attr.storage_type)) {
      case AttrStorageType::Array: {
        BLO_read_struct(&reader, AttributeDataArray, &dna_attr.data);
        auto &data = *static_cast<AttributeDataArray *>(dna_attr.data);
        read_attribute_data_array(reader, AttrType(dna_attr.data_type), data);
        attribute->data_ = Attribute::ArrayData{data.data, data.elements_num, data.sharing_info};
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

void AttributeStorage::blend_write_prepare(AttributeStorage::BlendWriteData &write_data)
{
  const Span<std::unique_ptr<Attribute>> attributes = this->runtime->attributes.as_span();

  write_data.attribute_ptrs.resize(attributes.size());
  write_data.attibutes.resize(attributes.size());
  write_data.array_data.resize(attributes.size());

  for (const int i : attributes.index_range()) {
    write_data.attribute_ptrs[i] = &write_data.attibutes[i];
    write_data.attibutes[i].name = attributes[i]->name().c_str();
    write_data.attibutes[i].domain = int8_t(attributes[i]->domain_);
    write_data.attibutes[i].data_type = int8_t(attributes[i]->data_type_);
    if (const auto *data = std::get_if<Attribute::ArrayData>(&attributes[i]->data_)) {
      write_data.attibutes[i].storage_type = int8_t(AttrStorageType::Array);
      write_data.attibutes[i].data = &write_data.array_data[i];
      write_data.array_data[i].data = data->data;
      write_data.array_data[i].elements_num = data->elements_num;
      write_data.array_data[i].sharing_info = data->sharing_info;
    }
  }

  this->attributes_array = write_data.attribute_ptrs.data();
  this->attributes_num = attributes.size();
}

void AttributeStorage::blend_write(BlendWriter &writer,
                                   const AttributeStorage::BlendWriteData & /*write_data*/)
{
  BLO_write_pointer_array(&writer, this->attributes_num, this->attributes_array);
  for (const ::Attribute *attribute : Span(this->attributes_array, this->attributes_num)) {
    BLO_write_struct(&writer, ::Attribute, attribute);
    BLO_write_string(&writer, attribute->name);
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
  this->attributes_array = nullptr;
  this->attributes_num = 0;
}

}  // namespace blender::bke
