/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"

namespace blender::nodes {

class List_For_GArray : public List {
  GArray<> array_;

 public:
  List_For_GArray(GArray<> array) : array_(std::move(array)) {}
  virtual ~List_For_GArray() {}

  GSpan values() const override
  {
    return array_.as_span();
  }
  GMutableSpan values_for_write() override
  {
    BLI_assert(this->is_mutable());
    return array_.as_mutable_span();
  }
};

ListPtr List::for_garray(GArray<> array)
{
  return ListPtr(MEM_new<List_For_GArray>(__func__, std::move(array)));
}

class List_For_Data : public List {
  const CPPType &cpp_type_;
  void *data_;
  int64_t size_;

 public:
  List_For_Data(const CPPType &cpp_type, void *data, const int64_t size)
      : cpp_type_(cpp_type), data_(data), size_(size)
  {
  }
  virtual ~List_For_Data() {}

  GSpan values() const override
  {
    return GSpan(cpp_type_, data_, size_);
  }
  GMutableSpan values_for_write() override
  {
    BLI_assert(this->is_mutable());
    return GMutableSpan(cpp_type_, data_, size_);
  }
};

ListPtr List::ForUninitialized(const CPPType &cpp_type, int64_t size)
{
  void *data = MEM_malloc_arrayN_aligned(size, cpp_type.size, cpp_type.alignment, __func__);
  return ListPtr(MEM_new<List_For_Data>(__func__, cpp_type, data, size));
}

void List::delete_self()
{
  MEM_delete(this);
}

}  // namespace blender::nodes
