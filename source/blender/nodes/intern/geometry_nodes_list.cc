/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"

namespace blender::nodes {

List::ArrayData List::ArrayData::ForValue(const GPointer &value, const int64_t size)
{
  List::ArrayData data{};
  const CPPType &type = *value.type();
  const void *value_ptr = type.default_value();

  /* Prefer `calloc` to zeroing after allocation since it is faster. */
  if (BLI_memory_is_zero(value_ptr, type.size)) {
    data.data = MEM_calloc_arrayN_aligned(size, type.size, type.alignment, __func__);
  }
  else {
    data.data = MEM_malloc_arrayN_aligned(size, type.size, type.alignment, __func__);
    type.fill_construct_n(value_ptr, data.data, size);
  }

  BLI_assert(type.is_trivially_destructible);
  data.sharing_info = ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data.data));
  return data;
}

List::ArrayData List::ArrayData::ForDefaultValue(const CPPType &type, const int64_t size)
{
  return ForValue(GPointer(type, type.default_value()), size);
}

List::ArrayData List::ArrayData::ForConstructed(const CPPType &type, const int64_t size)
{
  List::ArrayData data{};
  data.data = MEM_malloc_arrayN_aligned(size, type.size, type.alignment, __func__);
  type.default_construct_n(data.data, size);
  BLI_assert(type.is_trivially_destructible);
  data.sharing_info = ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data.data));
  return data;
}

List::ArrayData List::ArrayData::ForUninitialized(const CPPType &type, const int64_t size)
{
  List::ArrayData data{};
  data.data = MEM_malloc_arrayN_aligned(size, type.size, type.alignment, __func__);
  BLI_assert(type.is_trivially_destructible);
  data.sharing_info = ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data.data));
  return data;
}

List::SingleData List::SingleData::ForValue(const GPointer &value)
{
  List::SingleData data{};
  const CPPType &type = *value.type();
  data.value = MEM_mallocN_aligned(type.size, type.alignment, __func__);
  type.copy_construct(value.get(), data.value);
  BLI_assert(type.is_trivially_destructible);
  data.sharing_info = ImplicitSharingPtr<>(implicit_sharing::info_for_mem_free(data.value));
  return data;
}

List::SingleData List::SingleData::ForDefaultValue(const CPPType &type)
{
  return ForValue(GPointer(type, type.default_value()));
}

void List::delete_self()
{
  MEM_delete(this);
}

}  // namespace blender::nodes
