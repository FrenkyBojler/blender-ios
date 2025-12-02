/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BLI_generic_pointer.hh"
#include "BLI_generic_virtual_array.hh"

#include "NOD_geometry_nodes_list_fwd.hh"

namespace blender::nodes {

class List : public ImplicitSharingMixin {
 public:
  class ArrayData {
   public:
    const void *data;
    ImplicitSharingPtr<> sharing_info;
    static ArrayData ForValue(const GPointer &value, int64_t size);
    static ArrayData ForDefaultValue(const CPPType &type, int64_t size);
    static ArrayData ForConstructed(const CPPType &type, int64_t size);
    static ArrayData ForUninitialized(const CPPType &type, int64_t size);

    GMutableSpan span_for_write(const CPPType &type, int64_t size);
  };

  class SingleData {
   public:
    const void *value;
    ImplicitSharingPtr<> sharing_info;
    static SingleData ForValue(const GPointer &value);
    static SingleData ForDefaultValue(const CPPType &type);

    GMutablePointer value_for_write(const CPPType &type);
  };

  using DataVariant = std::variant<ArrayData, SingleData>;

 private:
  const CPPType &cpp_type_;
  DataVariant data_;
  int64_t size_ = 0;

 public:
  explicit List(const CPPType &type, DataVariant data, const int64_t size)
      : cpp_type_(type), data_(std::move(data)), size_(size)
  {
  }

  static ListPtr create(const CPPType &type, DataVariant data, const int64_t size)
  {
    return ListPtr(MEM_new<List>(__func__, type, std::move(data), size));
  }

  static List &ensure_mutable_inplace(ListPtr &list_ptr);

  DataVariant &data();
  const DataVariant &data() const;
  const CPPType &cpp_type() const;
  int64_t size() const;

  std::variant<GSpan, GPointer> values() const;
  std::variant<GMutableSpan, GMutablePointer> values_for_write();
  template<typename T> std::variant<Span<T>, const T *> values() const;
  template<typename T> std::variant<MutableSpan<T>, T *> values_for_write();

  template<typename T, typename Fn> void foreach(Fn &&fn) const;
  template<typename T, typename Fn> void foreach_for_write(Fn &&fn);

  ListPtr copy() const;

  void delete_self() override;

  /** Access the list as virtual array. */
  GVArray varray() const;
  template<typename T> VArray<T> varray() const;
};

inline const List::DataVariant &List::data() const
{
  return data_;
}

inline List::DataVariant &List::data()
{
  return data_;
}

inline const CPPType &List::cpp_type() const
{
  return cpp_type_;
}

inline int64_t List::size() const
{
  return size_;
}

template<typename T> inline VArray<T> List::varray() const
{
  return this->varray().typed<T>();
}

template<typename T> inline std::variant<Span<T>, const T *> List::values() const
{
  const std::variant<GSpan, GPointer> values = this->values();
  if (const auto *span_values = std::get_if<GSpan>(&values)) {
    return span_values->typed<T>();
  }
  if (const auto *single_value = std::get_if<GPointer>(&values)) {
    return single_value->get<T>();
  }
  BLI_assert_unreachable();
  return {};
}

template<typename T> inline std::variant<MutableSpan<T>, T *> List::values_for_write()
{
  const std::variant<GMutableSpan, GMutablePointer> values = this->values_for_write();
  if (const auto *span_values = std::get_if<GMutableSpan>(&values)) {
    return span_values->typed<T>();
  }
  if (const auto *single_value = std::get_if<GMutablePointer>(&values)) {
    return single_value->get<T>();
  }
  BLI_assert_unreachable();
  return {};
}

template<typename T, typename Fn> inline void List::foreach(Fn &&fn) const
{
  const std::variant<Span<T>, const T *> values = this->values<T>();
  if (const auto *span_values = std::get_if<Span<T>>(&values)) {
    for (const T &value : *span_values) {
      fn(value);
    }
  }
  else if (const auto *single_value = std::get_if<const T *>(&values)) {
    fn(**single_value);
  }
}
template<typename T, typename Fn> inline void List::foreach_for_write(Fn &&fn)
{
  const std::variant<MutableSpan<T>, T *> values = this->values_for_write<T>();
  if (auto *span_values = std::get_if<MutableSpan<T>>(&values)) {
    for (T &value : *span_values) {
      fn(value);
    }
  }
  else if (auto *single_value = std::get_if<T *>(&values)) {
    fn(**single_value);
  }
}

}  // namespace blender::nodes
