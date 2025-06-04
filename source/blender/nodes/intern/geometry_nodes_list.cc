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

void List::delete_self()
{
  MEM_delete(this);
}

}  // namespace blender::nodes
