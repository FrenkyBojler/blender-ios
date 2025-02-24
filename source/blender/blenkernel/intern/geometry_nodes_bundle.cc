/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_nodes_bundle.hh"

namespace blender::bke {

SocketInterfaceKey::SocketInterfaceKey(std::string identifier)
{
  identifiers_.append(std::move(identifier));
}

Span<std::string> SocketInterfaceKey::identifiers() const
{
  return identifiers_;
}

bool SocketInterfaceKey::matches(const SocketInterfaceKey &other) const
{
  for (const std::string &identifier : other.identifiers_) {
    if (identifiers_.contains(identifier)) {
      return true;
    }
  }
  return false;
}

Bundle::Bundle() = default;

Bundle::~Bundle()
{
  for (Item &item : items_) {
    item.value.destruct();
  }
  for (void *buffer : buffers_) {
    MEM_freeN(buffer);
  }
}

Bundle::Bundle(const Bundle &other)
{
  for (const Item &item : other.items_) {
    this->add_new(item.key, *item.value.type(), item.value.get());
  }
}

Bundle::Bundle(Bundle &&other) noexcept
    : items_(std::move(other.items_)), buffers_(std::move(other.buffers_))
{
}

Bundle &Bundle::operator=(const Bundle &other)
{
  if (this == &other) {
    return *this;
  }
  this->~Bundle();
  new (this) Bundle(other);
  return *this;
}

Bundle &Bundle::operator=(Bundle &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  this->~Bundle();
  new (this) Bundle(std::move(other));
  return *this;
}

void Bundle::add_new(SocketInterfaceKey key, const CPPType &type, const void *value)
{
  BLI_assert(!this->contains(key));
  void *buffer = MEM_mallocN_aligned(type.size(), type.alignment(), __func__);
  type.copy_construct(value, buffer);
  items_.append(Item{std::move(key), GMutablePointer(&type, buffer)});
  buffers_.append(buffer);
}

bool Bundle::add(const SocketInterfaceKey &key, const CPPType &type, const void *value)
{
  if (this->contains(key)) {
    return false;
  }
  this->add_new(key, type, value);
  return true;
}

bool Bundle::add(SocketInterfaceKey &&key, const CPPType &type, const void *value)
{
  if (this->contains(key)) {
    return false;
  }
  this->add_new(std::move(key), type, value);
  return true;
}

GPointer Bundle::lookup(const SocketInterfaceKey &key) const
{
  for (const Item &item : items_) {
    if (item.key.matches(key)) {
      return GPointer(item.value);
    }
  }
  return GPointer();
}

GMutablePointer Bundle::lookup_for_write(const SocketInterfaceKey &key)
{
  for (Item &item : items_) {
    if (item.key.matches(key)) {
      return item.value;
    }
  }
  return {};
}

bool Bundle::remove(const SocketInterfaceKey &key)
{
  const int removed_num = items_.remove_if([&key](Item &item) {
    if (item.key.matches(key)) {
      item.value.destruct();
      return true;
    }
    return false;
  });
  return removed_num >= 1;
}

bool Bundle::contains(const SocketInterfaceKey &key) const
{
  for (const Item &item : items_) {
    if (item.key.matches(key)) {
      return true;
    }
  }
  return false;
}

void Bundle::delete_self()
{
  MEM_delete(this);
}

}  // namespace blender::bke
