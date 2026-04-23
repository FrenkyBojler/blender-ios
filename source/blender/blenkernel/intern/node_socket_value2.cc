/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"

#include "FN_field.hh"

namespace blender::bke {

using fn::Field;
using fn::GField;

namespace detail {

template<>
void SocketValueVariantTypeInfo::convert_to_fn<int>(const CPPType &dst_type,
                                                    SocketValueVariantAny &value)
{
  const int v = value.get<int>();
  if (dst_type.is<float>()) {
    value.emplace<float>(v);
  }
  else if (dst_type.is<Field<int>>()) {
    value.emplace<GField>(Field<int>(v));
  }
  else if (dst_type.is<GField>()) {
    value.emplace<GField>(Field<int>(v));
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

template<>
void SocketValueVariantTypeInfo::convert_to_fn<GField>(const CPPType &dst_type,
                                                       SocketValueVariantAny &value)
{
  if (dst_type.is<GField>()) {
    return;
  }
  const CPPType &base_type = value.get<GField>().cpp_type();
  if (dst_type.is<Field<int>>()) {
    if (base_type.is<int>()) {
      return;
    }
  }

  SocketValueVariant2::init_default(dst_type, value);
}

template<>
bool SocketValueVariantTypeInfo::is_interpretable_as_fn<GField>(const CPPType &dst_type,
                                                                const SocketValueVariantAny &value)
{
  if (dst_type.is<GField>()) {
    return true;
  }
  const GField &field = value.get<GField>();
  if (dst_type.is<fn::Field<int>>()) {
    return field.cpp_type().is<int>();
  }
  return false;
}

}  // namespace detail

template<typename T>
inline T &SocketValueVariant2::init_default(detail::SocketValueVariantAny &value)
{
  if constexpr (detail::has_generic_type<T>) {
    using GenericT = T::generic_type;
    using BaseT = T::base_type;
    const CPPType &base_cpp_type = CPPType::get<BaseT>();
    return reinterpret_cast<T &>(value.emplace<GenericT>(base_cpp_type));
  }
  else if constexpr (std::is_same_v<T, GField>) {
    /* Some default fallback type. */
    return value.emplace<GField>(CPPType::get<float>());
  }
  else {
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
  if (type.is<GField>()) {
    return &SocketValueVariant2::init_default<GField>(value);
  }
  if (type.is<Field<int>>()) {
    return &SocketValueVariant2::init_default<Field<int>>(value);
  }
  return nullptr;
}

}  // namespace blender::bke
