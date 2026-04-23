/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"

namespace blender::bke {

namespace detail {

template<>
void SocketValueVariantTypeInfo::convert_to_fn<int>(const CPPType &dst_type,
                                                    SocketValueVariantAny &value)
{
  if (dst_type.is<float>()) {
    value.emplace<float>(value.get<int>());
  }
  else {
    SocketValueVariant2::init_default(dst_type, value);
  }
}

template<>
bool SocketValueVariantTypeInfo::is_interpretable_as_fn<int>(
    const CPPType &dst_type, const SocketValueVariantAny & /*value*/)
{
  return dst_type.is<int>();
}

template<>
void SocketValueVariantTypeInfo::convert_to_fn<float>(const CPPType &dst_type,
                                                      SocketValueVariantAny &value)
{
  if (dst_type.is<int>()) {
    const float v = value.get<float>();
    value.emplace<int>(v);
  }
  else {
    SocketValueVariant2::init_default(dst_type, value);
  }
}

template<>
bool SocketValueVariantTypeInfo::is_interpretable_as_fn<float>(
    const CPPType &dst_type, const SocketValueVariantAny & /*value*/)
{
  return dst_type.is<float>();
}

}  // namespace detail

template<typename T>
inline T &SocketValueVariant2::init_default(detail::SocketValueVariantAny &value)
{
  using StorageT = to_storage_type<T>;
  if constexpr (std::is_same_v<T, StorageT>) {
    return value.emplace<T>();
  }
}

void *SocketValueVariant2::init_default(const CPPType &type, detail::SocketValueVariantAny &value)
{
  if (type.is<float>()) {
    return &SocketValueVariant2::init_default<float>(value);
  }
  if (type.is<int>()) {
    return &SocketValueVariant2::init_default<int>(value);
  }
  return nullptr;
}

}  // namespace blender::bke
