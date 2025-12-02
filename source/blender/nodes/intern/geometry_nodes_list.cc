/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"

namespace blender::nodes {

class ArrayImplicitSharingData : public ImplicitSharingInfo {
 public:
  const CPPType &type;
  void *data;
  int64_t size;

  ArrayImplicitSharingData(void *data, const int64_t size, const CPPType &type)
      : ImplicitSharingInfo(), type(type), data(data), size(size)
  {
  }

 private:
  void delete_self_with_data() override
  {
    type.destruct_n(this->data, this->size);
    MEM_freeN(this->data);
    MEM_delete(this);
  }
};

static ImplicitSharingPtr<> sharing_ptr_for_array(void *data,
                                                  const int64_t size,
                                                  const CPPType &type)
{
  if (type.is_trivially_destructible) {
    /* Avoid storing size and type in sharing info if unnecessary. */
    return ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data));
  }
  return ImplicitSharingPtr<>(MEM_new<ArrayImplicitSharingData>(__func__, data, size, type));
}

List::ArrayData List::ArrayData::ForValue(const GPointer &value, const int64_t size)
{
  List::ArrayData data{};
  const CPPType &type = *value.type();
  const void *value_ptr = type.default_value();

  void *new_data;
  /* Prefer `calloc` to zeroing after allocation since it is faster. */
  if (BLI_memory_is_zero(value_ptr, type.size)) {
    new_data = MEM_calloc_arrayN_aligned(size, type.size, type.alignment, __func__);
  }
  else {
    new_data = MEM_malloc_arrayN_aligned(size, type.size, type.alignment, __func__);
    type.fill_construct_n(value_ptr, new_data, size);
  }
  data.data = new_data;

  data.sharing_info = sharing_ptr_for_array(new_data, size, type);
  return data;
}

List::ArrayData List::ArrayData::ForDefaultValue(const CPPType &type, const int64_t size)
{
  return ForValue(GPointer(type, type.default_value()), size);
}

List::ArrayData List::ArrayData::ForConstructed(const CPPType &type, const int64_t size)
{
  List::ArrayData data{};
  void *new_data = MEM_malloc_arrayN_aligned(size, type.size, type.alignment, __func__);
  type.default_construct_n(new_data, size);
  data.data = new_data;
  data.sharing_info = sharing_ptr_for_array(new_data, size, type);
  return data;
}

List::ArrayData List::ArrayData::ForUninitialized(const CPPType &type, const int64_t size)
{
  List::ArrayData data{};
  void *new_data = MEM_malloc_arrayN_aligned(size, type.size, type.alignment, __func__);
  data.data = new_data;
  data.sharing_info = sharing_ptr_for_array(new_data, size, type);
  return data;
}

GMutableSpan List::ArrayData::span_for_write(const CPPType &type, int64_t size)
{
  if (this->sharing_info && !this->sharing_info->is_mutable()) {
    void *new_data = MEM_malloc_arrayN(size, type.size, __func__);
    type.copy_construct_n(this->data, new_data, size);
    this->data = new_data;
    this->sharing_info = sharing_ptr_for_array(new_data, size, type);
  }
  if (this->sharing_info) {
    this->sharing_info->tag_ensured_mutable();
  }
  return {type, const_cast<void *>(this->data), size};
}

std::variant<GSpan, GPointer> List::values() const
{
  if (const auto *array_data = std::get_if<ArrayData>(&data_)) {
    return GSpan(cpp_type_, array_data->data, size_);
  }
  if (const auto *single_data = std::get_if<SingleData>(&data_)) {
    return GPointer(cpp_type_, single_data->value);
  }
  BLI_assert_unreachable();
  return {};
}

std::variant<GMutableSpan, GMutablePointer> List::values_for_write()
{
  if (auto *array_data = std::get_if<ArrayData>(&data_)) {
    return array_data->span_for_write(cpp_type_, size_);
  }
  if (auto *single_data = std::get_if<SingleData>(&data_)) {
    return single_data->value_for_write(cpp_type_);
  }
  BLI_assert_unreachable();
  return {};
}

class SingleImplicitSharingData : public ImplicitSharingInfo {
 public:
  const CPPType &type;
  void *data;

  SingleImplicitSharingData(void *data, const CPPType &type)
      : ImplicitSharingInfo(), type(type), data(data)
  {
  }

 private:
  void delete_self_with_data() override
  {
    type.destruct(this->data);
    MEM_delete(this);
  }
};

static ImplicitSharingPtr<> sharing_ptr_for_value(void *data, const CPPType &type)
{
  if (type.is_trivially_destructible) {
    /* Avoid storing size and type in sharing info if unnecessary. */
    return ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data));
  }
  return ImplicitSharingPtr<>(MEM_new<SingleImplicitSharingData>(__func__, data, type));
}

List::SingleData List::SingleData::ForValue(const GPointer &value)
{
  List::SingleData data{};
  const CPPType &type = *value.type();
  void *new_value = MEM_mallocN_aligned(type.size, type.alignment, __func__);
  type.copy_construct(value.get(), new_value);
  data.value = new_value;
  data.sharing_info = sharing_ptr_for_value(new_value, type);
  return data;
}

List::SingleData List::SingleData::ForDefaultValue(const CPPType &type)
{
  return ForValue(GPointer(type, type.default_value()));
}

GMutablePointer List::SingleData::value_for_write(const CPPType &type)
{
  if (this->sharing_info && !this->sharing_info->is_mutable()) {
    void *new_data = MEM_mallocN(type.size, __func__);
    type.copy_construct(this->value, new_data);
    this->value = new_data;
    this->sharing_info = sharing_ptr_for_value(new_data, type);
  }
  if (this->sharing_info) {
    this->sharing_info->tag_ensured_mutable();
  }
  return GMutablePointer{type, const_cast<void *>(this->value)};
}

void List::delete_self()
{
  MEM_delete(this);
}

GVArray List::varray() const
{
  if (const auto *array_data = std::get_if<ArrayData>(&data_)) {
    return GVArray::from_span(GSpan(cpp_type_, array_data->data, size_));
  }
  if (const auto *single_data = std::get_if<SingleData>(&data_)) {
    return GVArray::from_single_ref(cpp_type_, size_, single_data->value);
  }
  BLI_assert_unreachable();
  return {};
}

ListPtr List::copy() const
{
  return nodes::List::create(this->cpp_type(), this->data(), this->size());
}

List &List::ensure_mutable_inplace(ListPtr &list_ptr)
{
  BLI_assert(list_ptr);
  if (!list_ptr->is_mutable()) {
    list_ptr = list_ptr->copy();
  }
  BLI_assert(list_ptr->is_mutable());
  list_ptr->tag_ensured_mutable();
  return const_cast<List &>(*list_ptr);
}

}  // namespace blender::nodes
