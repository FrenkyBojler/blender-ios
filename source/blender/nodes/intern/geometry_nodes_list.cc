/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"

namespace blender::nodes {

List::List() = default;

List::~List() {}

List::List(const List &other) : values_(other.values_), sharing_info_(other.sharing_info_) {}

List::List(List &&other) noexcept
    : values_(other.values_), sharing_info_(std::move(other.sharing_info_))
{
}

List &List::operator=(const List &other)
{
  if (this == &other) {
    return *this;
  }
  this->~List();
  new (this) List(other);
  return *this;
}

List &List::operator=(List &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  this->~List();
  new (this) List(std::move(other));
  return *this;
}

void List::delete_self()
{
  MEM_delete(this);
}

}  // namespace blender::nodes
