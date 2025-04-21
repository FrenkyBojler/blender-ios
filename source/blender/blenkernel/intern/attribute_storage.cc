/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_assert.h"
#include "BLI_implicit_sharing.hh"
#include "BLI_string_utils.hh"
#include "BLI_vector_set.hh"

#include "BLO_read_write.hh"

#include "DNA_attribute_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_attribute_storage.hh"
#include "BKE_attribute_storage_blend_write.hh"

namespace blender::bke {

/**
 * \note There is a possibility to support some caches here, like the min and max values of the
 * array.
 */
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

void AttributeStorage::foreach(FunctionRef<void(Attribute &)> fn)
{
  for (const std::unique_ptr<Attribute> &attribute : this->runtime->attributes) {
    fn(*attribute);
  }
}
void AttributeStorage::foreach(FunctionRef<void(const Attribute &)> fn) const
{
  for (const std::unique_ptr<Attribute> &attribute : this->runtime->attributes) {
    fn(*attribute);
  }
}

static ImplicitSharingInfo *create_sharing_info_for_array(void *data,
                                                          const int64_t size,
                                                          const CPPType &type)
{
  return MEM_new<ArrayDataImplicitSharing>(__func__, data, size, type);
}

AttrStorageType Attribute::storage_type() const
{
  if (std::get_if<Attribute::ArrayData>(&data_)) {
    return AttrStorageType::Array;
  }
  if (std::get_if<Attribute::SingleData>(&data_)) {
    return AttrStorageType::Single;
  }
  BLI_assert_unreachable();
  return AttrStorageType::Array;
}

Attribute::DataVariant &Attribute::data_for_write()
{
  if (auto *data = std::get_if<Attribute::ArrayData>(&data_)) {
    if (data->sharing_info->is_mutable()) {
      data->sharing_info->tag_ensured_mutable();
      return data_;
    }

    const CPPType &cpp_type = attribute_type_to_cpp_type(type_);
    void *new_data = MEM_malloc_arrayN_aligned(
        data->size, cpp_type.size, cpp_type.alignment, __func__);
    cpp_type.copy_construct_n(data->data, new_data, data->size);

    data->data = new_data;
    data->sharing_info = ImplicitSharingPtr<>(
        create_sharing_info_for_array(data->data, data->size, cpp_type));
  }
  else if (std::get_if<Attribute::SingleData>(&data_)) {
    /* Not yet implemented because #SingleData isn't used at runtime yet. */
    BLI_assert_unreachable();
  }
  return data_;
}

AttributeStorage::AttributeStorage()
{
  this->dna_attributes = nullptr;
  this->dna_attributes_num = 0;
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
}

AttributeStorage::AttributeStorage(const AttributeStorage &other)
{
  this->dna_attributes = nullptr;
  this->dna_attributes_num = 0;
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
  this->runtime->attributes.reserve(other.runtime->attributes.size());
  other.foreach([&](const Attribute &attribute) {
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
  this->dna_attributes = nullptr;
  this->dna_attributes_num = 0;
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

Attribute &AttributeStorage::add(std::string name,
                                 const AttrDomain domain,
                                 const AttrType data_type,
                                 Attribute::ArrayData data)
{
  return this->add(name, domain, data_type, std::move(data));
}

std::string AttributeStorage::unique_name_calc(const StringRef name)
{
  return BLI_uniquename_cb(
      [&](const StringRef check_name) { return this->lookup(check_name) != nullptr; }, '.', name);
}

Attribute &AttributeStorage::add(std::string name,
                                 const AttrDomain domain,
                                 const AttrType data_type,
                                 Attribute::DataVariant &&data)
{
  BLI_assert(!this->lookup(name));
  std::unique_ptr<Attribute> ptr = std::make_unique<Attribute>();
  Attribute &attribute = *ptr;
  attribute.name_ = std::move(name);
  attribute.domain_ = domain;
  attribute.type_ = data_type;
  attribute.data_ = std::move(data);
  this->runtime->attributes.add_new(std::move(ptr));
  return attribute;
}

static void read_array_data(BlendDataReader &reader,
                            const AttrType data_type,
                            const int64_t size,
                            void **data)
{
  switch (data_type) {
    case AttrType::Bool:
      static_assert(sizeof(bool) == sizeof(int8_t));
      BLO_read_int8_array(&reader, size, reinterpret_cast<int8_t **>(data));
      break;
    case AttrType::Int8:
      BLO_read_int8_array(&reader, size, reinterpret_cast<int8_t **>(data));
      break;
    case AttrType::Int16_2D:
      BLO_read_int16_array(&reader, size * 2, reinterpret_cast<int16_t **>(data));
      break;
    case AttrType::Int32:
      BLO_read_int32_array(&reader, size, reinterpret_cast<int32_t **>(data));
      break;
    case AttrType::Int32_2D:
      BLO_read_int32_array(&reader, size * 2, reinterpret_cast<int32_t **>(data));
      break;
    case AttrType::Float:
      BLO_read_float_array(&reader, size, reinterpret_cast<float **>(data));
      break;
    case AttrType::Float2:
      BLO_read_float_array(&reader, size * 2, reinterpret_cast<float **>(data));
      break;
    case AttrType::Float3:
      BLO_read_float3_array(&reader, size, reinterpret_cast<float **>(data));
      break;
    case AttrType::Float4x4:
      BLO_read_float_array(&reader, size * 16, reinterpret_cast<float **>(data));
      break;
    case AttrType::ColorByte:
      BLO_read_uint8_array(&reader, size * 4, reinterpret_cast<uint8_t **>(data));
      break;
    case AttrType::ColorFloat:
      BLO_read_float_array(&reader, size * 4, reinterpret_cast<float **>(data));
      break;
    case AttrType::Quaternion:
      BLO_read_float_array(&reader, size * 4, reinterpret_cast<float **>(data));
      break;
    case AttrType::String:
      BLO_read_struct_array(
          &reader, MStringProperty, size, reinterpret_cast<MStringProperty **>(data));
      break;
  }
}

static void read_shared_array(BlendDataReader &reader,
                              const AttrType data_type,
                              const int64_t size,
                              void **data,
                              const ImplicitSharingInfo **sharing_info)
{
  const char *func = __func__;
  *sharing_info = BLO_read_shared(&reader, &data, [&]() -> const ImplicitSharingInfo * {
    read_array_data(reader, data_type, size, data);
    const CPPType &cpp_type = attribute_type_to_cpp_type(data_type);
    return MEM_new<ArrayDataImplicitSharing>(func, *data, size, cpp_type);
  });
}

void AttributeStorage::blend_read(BlendDataReader &reader)
{
  this->runtime = MEM_new<AttributeStorageRuntime>(__func__);
  this->runtime->attributes.reserve(this->dna_attributes_num);

  BLO_read_struct_array(&reader, AttributeDNA, this->dna_attributes_num, &this->dna_attributes);
  for (const int i : IndexRange(this->dna_attributes_num)) {
    AttributeDNA &dna_attr = this->dna_attributes[i];
    BLO_read_string(&reader, &dna_attr.name);

    std::unique_ptr<Attribute> attribute = std::make_unique<Attribute>();
    attribute->name_ = dna_attr.name;
    attribute->domain_ = AttrDomain(dna_attr.domain);
    attribute->type_ = AttrType(dna_attr.data_type);

    switch (AttrStorageType(dna_attr.storage_type)) {
      case AttrStorageType::Array: {
        BLO_read_struct(&reader, AttributeArrayDNA, &dna_attr.data);
        auto &data = *static_cast<AttributeArrayDNA *>(dna_attr.data);
        read_shared_array(reader, attribute->type_, data.size, &data.data, &data.sharing_info);
        attribute->data_ = Attribute::ArrayData{
            data.data, data.size, ImplicitSharingPtr<>(data.sharing_info)};
        break;
      }
      case AttrStorageType::Single: {
        BLO_read_struct(&reader, AttributeSingleDNA, &dna_attr.data);
        auto &data = *static_cast<AttributeSingleDNA *>(dna_attr.data);
        read_shared_array(reader, attribute->type_, 1, &data.data, &data.sharing_info);
        attribute->data_ = Attribute::SingleData{data.data,
                                                 ImplicitSharingPtr<>(data.sharing_info)};
        break;
      }
    }

    MEM_SAFE_FREE(dna_attr.name);
    MEM_SAFE_FREE(dna_attr.data);

    this->runtime->attributes.add_new(std::move(attribute));
  }

  /* These fields are not used at runtime. */
  MEM_SAFE_FREE(this->dna_attributes);
  this->dna_attributes_num = 0;
}

static void write_array_data(BlendWriter &writer,
                             const AttrType data_type,
                             const void *data,
                             const int64_t size)
{
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
}

void attribute_storage_blend_write_prepare(
    AttributeStorage &data,
    const Map<AttrDomain, Vector<CustomDataLayer, 16> *> &layers_to_write,
    AttributeStorage::BlendWriteData &write_data)
{
  Set<StringRef, 16> all_names_written;
  data.foreach([&](Attribute &attr) {
    if (!U.experimental.use_attribute_storage_write_debug) {
      /* In version 4.5, all attribute data is written in the #CustomData format (at least when the
       * debug option is not enabled), so the #Attribute needs to be converted to a
       * #CustomDataLayer in the proper list. This is only relevant when #AttributeStorage is
       * actually used at runtime. */
      if (const std::optional data_type = attr_type_to_custom_data_type(attr.data_type())) {
        if (const auto *array_data = std::get_if<Attribute::ArrayData>(&attr.data())) {
          CustomDataLayer layer{};
          layer.type = *data_type;
          layer.data = array_data->data;
          layer.sharing_info = array_data->sharing_info.get();

          /* Because the #Attribute::name_ `std::string` has no length limit (unlike
           * #CustomDataLayer::name), we have to manually make the name unique in case it exceeds
           * the limit. */
          BLI_uniquename_cb(
              [&](const StringRefNull name) { return all_names_written.contains(name); },
              attr.name().c_str(),
              '.',
              layer.name,
              MAX_CUSTOMDATA_LAYER_NAME);
          all_names_written.add(layer.name);

          layers_to_write.lookup(attr.domain())->append(layer);
        }
      }
      return;
    }

    all_names_written.add(attr.name());
    AttributeDNA attribute_dna{};
    attribute_dna.name = attr.name().c_str();
    attribute_dna.data_type = int16_t(attr.data_type());
    attribute_dna.domain = int8_t(attr.domain());
    attribute_dna.storage_type = int8_t(attr.storage_type());

    /* The idea is to use a separate DNA struct for each #AttrStorageType. They each need to have a
     * unique address (while writing a specific ID anyway) in order to be identified when
     * reading the file, so we add them to the resource scope which outlives this function call.
     * Using a #ResourceScope is a simple way to get pointer stability when adding every new data
     * struct without the cost of many small allocations or unnecessary overhead of storing a full
     * array for every storage type. */

    if (const auto *data = std::get_if<Attribute::ArrayData>(&attr.data())) {
      auto &array_dna = write_data.scope.construct<AttributeArrayDNA>();
      array_dna.data = data->data;
      array_dna.sharing_info = data->sharing_info.get();
      array_dna.size = data->size;
      attribute_dna.data = &array_dna;
    }
    else if (const auto *data = std::get_if<Attribute::SingleData>(&attr.data())) {
      auto &single_dna = write_data.scope.construct<AttributeSingleDNA>();
      single_dna.data = data->value;
      single_dna.sharing_info = data->sharing_info.get();
      attribute_dna.data = &single_dna;
    }

    write_data.attributes.append(attribute_dna);
  });
}

static void write_shared_array(BlendWriter &writer,
                               const AttrType data_type,
                               const void *data,
                               const int64_t size,
                               const ImplicitSharingInfo &sharing_info)
{
  const CPPType &cpp_type = attribute_type_to_cpp_type(data_type);
  BLO_write_shared(&writer, data, cpp_type.size * size, &sharing_info, [&]() {
    write_array_data(writer, data_type, data, size);
  });
}

void AttributeStorage::blend_write(BlendWriter &writer,
                                   const AttributeStorage::BlendWriteData &write_data)
{
  BLO_write_struct_array(
      &writer, AttributeDNA, write_data.attributes.size(), write_data.attributes.data());
  for (const AttributeDNA &attr_dna : write_data.attributes) {
    BLO_write_string(&writer, attr_dna.name);
    switch (AttrStorageType(attr_dna.storage_type)) {
      case AttrStorageType::Single: {
        AttributeSingleDNA *single_dna = static_cast<AttributeSingleDNA *>(attr_dna.data);
        BLO_write_struct(&writer, AttributeSingleDNA, single_dna);
        write_shared_array(
            writer, AttrType(attr_dna.data_type), single_dna->data, 1, *single_dna->sharing_info);
        break;
      }
      case AttrStorageType::Array: {
        AttributeArrayDNA *array_dna = static_cast<AttributeArrayDNA *>(attr_dna.data);
        BLO_write_struct(&writer, AttributeArrayDNA, array_dna);
        write_shared_array(writer,
                           AttrType(attr_dna.data_type),
                           array_dna->data,
                           array_dna->size,
                           *array_dna->sharing_info);
        break;
      }
    }
  }

  this->dna_attributes = nullptr;
  this->dna_attributes_num = 0;
}

}  // namespace blender::bke
