/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_node.hh"

#include "BKE_node_socket_value.hh"
#include "NOD_geometry_nodes_bundle_fwd.hh"
#include "NOD_geometry_nodes_values.hh"

#include "DNA_node_types.h"

namespace blender::nodes {

struct BundleItemSocketValue {
  const bke::bNodeSocketType *type;
  void *value;
};

struct BundleItemInternalValue {
  ImplicitSharingPtr<> data;
};

struct BundleItemValue {
  std::variant<BundleItemSocketValue, BundleItemInternalValue> value;

  /**
   * Attempts to cast the stored value to the given type. This may do implicit conversions.
   */
  template<typename T> std::optional<T> as(const bke::bNodeSocketType &socket_type) const;
  template<typename T> std::optional<T> as() const;
};

/**
 * A bundle is a map containing keys and their corresponding values. Values are stored as the type
 * they have in Geometry Nodes (#bNodeSocketType::geometry_nodes_cpp_type).
 *
 * The API also supports working with paths in nested bundles like `root/child/data`.
 */
class Bundle : public ImplicitSharingMixin {
 public:
  struct StoredItem {
    std::string key;
    BundleItemValue value;
  };

 private:
  Vector<StoredItem> items_;
  Vector<void *> buffers_;

 public:
  Bundle();
  Bundle(const Bundle &other);
  Bundle(Bundle &&other) noexcept;
  Bundle &operator=(const Bundle &other);
  Bundle &operator=(Bundle &&other) noexcept;
  ~Bundle();

  static BundlePtr create();

  bool add(StringRef key, const BundleItemValue &value);
  void add_new(StringRef key, const BundleItemValue &value);
  void add_override(StringRef key, const BundleItemValue &value);
  bool add_path(StringRef path, const BundleItemValue &value);
  void add_path_new(StringRef path, const BundleItemValue &value);
  void add_path_override(StringRef path, const BundleItemValue &value);

  template<typename T> void add(StringRef key, T value);
  template<typename T> void add_override(StringRef key, T value);
  template<typename T> void add_path(StringRef path, T value);
  template<typename T> void add_path_override(StringRef path, T value);

  bool remove(StringRef key);
  bool contains(StringRef key) const;
  bool contains_path(StringRef path) const;

  const BundleItemValue *lookup(StringRef key) const;
  const BundleItemValue *lookup_path(Span<StringRef> path) const;
  const BundleItemValue *lookup_path(StringRef path) const;
  template<typename T> std::optional<T> lookup(StringRef key) const;
  template<typename T> std::optional<T> lookup_path(StringRef path) const;

  bool is_empty() const;
  int64_t size() const;

  Span<StoredItem> items() const;

  BundlePtr copy() const;

  void delete_self() override;
};

template<typename T>
inline std::optional<T> BundleItemValue::as(const bke::bNodeSocketType &dst_socket_type) const
{
  const BundleItemSocketValue *socket_value = std::get_if<BundleItemSocketValue>(&this->value);
  if (!socket_value) {
    return std::nullopt;
  }
  if (!socket_value) {
    return std::nullopt;
  }
  if (!socket_value->value || !socket_value->type) {
    return std::nullopt;
  }
  const void *converted_value = socket_value->value;
  BUFFER_FOR_CPP_TYPE_VALUE(*dst_socket_type.geometry_nodes_cpp_type, buffer);
  if (socket_value->type != &dst_socket_type) {
    if (!implicitly_convert_socket_value(
            *socket_value->type, socket_value->value, dst_socket_type, buffer))
    {
      return std::nullopt;
    }
    converted_value = buffer;
  }
  if constexpr (geo_nodes_type_stored_as_SocketValueVariant_v<T>) {
    const auto &value_variant = *static_cast<const bke::SocketValueVariant *>(converted_value);
    return value_variant.get<T>();
  }
  return *static_cast<const T *>(converted_value);
}

template<typename T> constexpr bool is_valid_static_bundle_item_type()
{
  if (geo_nodes_is_field_base_type_v<T>) {
    return true;
  }
  if constexpr (fn::is_field_v<T>) {
    return geo_nodes_is_field_base_type_v<typename T::base_type>;
  }
  if constexpr (is_same_any_v<T, BundlePtr, ClosurePtr>) {
    return true;
  }
  return !geo_nodes_type_stored_as_SocketValueVariant_v<T>;
}

template<typename T> inline const bke::bNodeSocketType *socket_type_info_by_static_type()
{
  if constexpr (fn::is_field_v<T>) {
    if constexpr (geo_nodes_is_field_base_type_v<typename T::base_type>) {
      const std::optional<eNodeSocketDatatype> socket_type =
          bke::geo_nodes_base_cpp_type_to_socket_type(CPPType::get<typename T::base_type>());
      BLI_assert(socket_type);
      const bke::bNodeSocketType *socket_type_info = bke::node_socket_type_find_static(
          *socket_type);
      BLI_assert(socket_type_info);
      return socket_type_info;
    }
  }
  const std::optional<eNodeSocketDatatype> socket_type =
      bke::geo_nodes_base_cpp_type_to_socket_type(CPPType::get<T>());
  if (!socket_type) {
    return nullptr;
  }
  return bke::node_socket_type_find_static(*socket_type);
}

template<typename T> inline std::optional<T> BundleItemValue::as() const
{
  static_assert(is_valid_static_bundle_item_type<T>());
  if (const bke::bNodeSocketType *dst_socket_type = socket_type_info_by_static_type<T>()) {
    return this->as<T>(*dst_socket_type);
  }
  /* Can't lookup this type directly currently. */
  BLI_assert_unreachable();
  return std::nullopt;
}

template<typename T> inline std::optional<T> Bundle::lookup(const StringRef key) const
{
  const BundleItemValue *item = this->lookup(key);
  if (!item) {
    return std::nullopt;
  }
  return item->as<T>();
}

template<typename T> inline std::optional<T> Bundle::lookup_path(const StringRef path) const
{
  const BundleItemValue *item = this->lookup_path(path);
  if (!item) {
    return std::nullopt;
  }
  return item->as<T>();
}

template<typename T, typename Fn> inline void to_stored_type(T &&value, Fn &&fn)
{
  using DecayT = std::decay_t<T>;
  if constexpr (std::is_same_v<DecayT, BundleItemValue>) {
    fn(std::forward<T>(value));
  }
  else if constexpr (std::is_same_v<DecayT, BundleItemSocketValue>) {
    fn(BundleItemValue{std::forward<T>(value)});
  }
  else if constexpr (std::is_same_v<DecayT, BundleItemInternalValue>) {
    fn(BundleItemValue{std::forward<T>(value)});
  }
  else {
    static_assert(is_valid_static_bundle_item_type<DecayT>());
    const bke::bNodeSocketType *socket_type = socket_type_info_by_static_type<DecayT>();
    BLI_assert(socket_type);
    if constexpr (geo_nodes_type_stored_as_SocketValueVariant_v<DecayT>) {
      auto value_variant = bke::SocketValueVariant::From(std::forward<T>(value));
      fn(BundleItemValue{BundleItemSocketValue{socket_type, &value_variant}});
    }
    else {
      fn(BundleItemValue{BundleItemSocketValue{socket_type, &value}});
    }
  }
}

template<typename T> inline void Bundle::add(const StringRef key, T value)
{
  to_stored_type(value, [&](const BundleItemValue &item_value) { this->add(key, item_value); });
}

template<typename T> inline void Bundle::add_path(const StringRef path, T value)
{
  to_stored_type(value,
                 [&](const BundleItemValue &item_value) { this->add_path(path, item_value); });
}

template<typename T> inline void Bundle::add_override(const StringRef key, T value)
{
  to_stored_type(value,
                 [&](const BundleItemValue &item_value) { this->add_override(key, item_value); });
}

template<typename T> inline void Bundle::add_path_override(const StringRef path, T value)
{
  to_stored_type(value, [&](const BundleItemValue &item_value) {
    this->add_path_override(path, item_value);
  });
}

inline Span<Bundle::StoredItem> Bundle::items() const
{
  return items_;
}

inline bool Bundle::is_empty() const
{
  return items_.is_empty();
}

inline int64_t Bundle::size() const
{
  return items_.size();
}

}  // namespace blender::nodes
