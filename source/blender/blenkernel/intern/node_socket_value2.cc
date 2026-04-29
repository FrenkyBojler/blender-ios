/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"
#include "BKE_type_conversions.hh"

#include "FN_field.hh"
#include "FN_field_evaluation.hh"

namespace blender::bke {

using fn::Field;
using fn::GField;
using volume_grid::GVolumeGrid;

namespace detail {

template<typename CurrentT>
void SocketValueVariantTypeInfo::convert_to_fn(const CPPType &dst_type,
                                               SocketValueVariantAny &value)
{
  if (CPPType::get<CurrentT>() == dst_type) {
    return;
  }
  static const DataTypeConversions &conversions = get_implicit_type_conversions();
  if constexpr (std::is_same_v<CurrentT, GField>) {
    const GField &src_field = value.get<GField>();
    const CPPType &src_base_type = src_field.cpp_type();
    if (dst_type.generic_type && dst_type.generic_type->is<GField>()) {
      if (src_base_type == *dst_type.base_type) {
        /* Nothing to do.*/
        return;
      }
      const ConversionFunctions *fns = conversions.get_conversion_functions(src_base_type,
                                                                            *dst_type.base_type);
      if (!fns) {
        SocketValueVariant2::init_default(dst_type, value);
        return;
      }
      if (const void *src_single_value = src_field.get_if_constant()) {
        if (!fns->convert_single_to_initialized) {
          SocketValueVariant2::init_default(dst_type, value);
          return;
        }
        BUFFER_FOR_CPP_TYPE_VALUE(*dst_type.base_type, dst_single_value);
        fns->convert_single_to_initialized(src_single_value, dst_single_value);
        value.emplace<GField>(GField::from_constant(*dst_type.base_type, dst_single_value));
        dst_type.base_type->destruct(dst_single_value);
        return;
      }
      if (!fns->multi_function) {
        SocketValueVariant2::init_default(dst_type, value);
        return;
      }
      fn::FieldOperationPtr op = fn::FieldOperation::from(*fns->multi_function, {src_field});
      value.emplace<GField>(GField(std::move(op), 0));
      return;
    }

    if (src_base_type == dst_type) {
      BUFFER_FOR_CPP_TYPE_VALUE(dst_type, tmp_buffer);
      fn::evaluate_constant_field(src_field, tmp_buffer);
      void *dst_value = SocketValueVariant2::allocate(dst_type, value);
      dst_type.move_construct(tmp_buffer, dst_value);
      dst_type.destruct(tmp_buffer);
      return;
    }
    const ConversionFunctions *fns = conversions.get_conversion_functions(src_base_type, dst_type);
    if (!fns || !fns->convert_single_to_initialized) {
      SocketValueVariant2::init_default(dst_type, value);
      return;
    }
    BUFFER_FOR_CPP_TYPE_VALUE(src_base_type, src_single_value);
    fn::evaluate_constant_field(src_field, src_single_value);
    void *dst_value = SocketValueVariant2::allocate(dst_type, value);
    fns->convert_single_to_uninitialized(src_single_value, dst_value);
    src_base_type.destruct(src_single_value);
    return;
  }
  else if constexpr (std::is_same_v<CurrentT, volume_grid::GVolumeGrid>) {
    // TODO
    SocketValueVariant2::init_default(dst_type, value);
  }
  else if constexpr (std::is_same_v<CurrentT, nodes::List>) {
    // TODO
    SocketValueVariant2::init_default(dst_type, value);
  }
  else {
    /* The stored value is a single value. */

    if (dst_type.is<GField>()) {
      GField field = GField::from_constant(CPPType::get<CurrentT>(), value.get());
      value.emplace<GField>(std::move(field));
      return;
    }
    if (dst_type.generic_type && dst_type.generic_type->is<GField>()) {
      if (dst_type.base_type->is<CurrentT>()) {
        GField field = GField::from_constant(CPPType::get<CurrentT>(), value.get());
        value.emplace<GField>(std::move(field));
        return;
      }
      const ConversionFunctions *fns = conversions.get_conversion_functions(
          CPPType::get<CurrentT>(), *dst_type.base_type);
      if (!fns || !fns->convert_single_to_initialized) {
        SocketValueVariant2::init_default(dst_type, value);
        return;
      }
      BUFFER_FOR_CPP_TYPE_VALUE(*dst_type.base_type, tmp_buffer);
      fns->convert_single_to_initialized(value.get(), tmp_buffer);
      value.emplace<GField>(GField::from_constant(*dst_type.base_type, tmp_buffer));
      dst_type.base_type->destruct(tmp_buffer);
      return;
    }
    const ConversionFunctions *fns = conversions.get_conversion_functions(CPPType::get<CurrentT>(),
                                                                          dst_type);
    if (!fns || !fns->convert_single_to_uninitialized) {
      SocketValueVariant2::init_default(dst_type, value);
      return;
    }
    BUFFER_FOR_CPP_TYPE_VALUE(dst_type, tmp_buffer);
    fns->convert_single_to_uninitialized(value.get(), tmp_buffer);
    void *dst_value = SocketValueVariant2::allocate(dst_type, value);
    dst_type.move_construct(tmp_buffer, dst_value);
    dst_type.destruct(tmp_buffer);
  }
}

template<typename CurrentT>
bool SocketValueVariantTypeInfo::is_interpretable_as_fn(const CPPType &dst_type,
                                                        const SocketValueVariantAny &value)
{
  /* Handles fields and volume grids. */
  if constexpr (requires { typename CurrentT::generic_type; }) {
    if (dst_type.is<CurrentT>()) {
      return true;
    }
    if (!dst_type.generic_type) {
      return false;
    }
    if (dst_type.generic_type->is<CurrentT>()) {
      const CurrentT &generic_value = value.get<CurrentT>();
      const CPPType &base_type = generic_value.cpp_type();
      BLI_assert(dst_type.base_type);
      return base_type == *dst_type.base_type;
    }
    return false;
  }
  else if constexpr (std::is_same_v<CurrentT, nodes::List>) {
    return dst_type.is<nodes::List>();
  }
  else {
    return CPPType::get<CurrentT>() == dst_type;
  }
}

#define DEFINE_TYPE(TYPE) \
  template void SocketValueVariantTypeInfo::convert_to_fn<TYPE>(const CPPType &dst_type, \
                                                                SocketValueVariantAny &value); \
  template bool SocketValueVariantTypeInfo::is_interpretable_as_fn<TYPE>( \
      const CPPType &dst_type, const SocketValueVariantAny &value);

/* Might not be strictly necessary for types used in this file. */
DEFINE_TYPE(int)
DEFINE_TYPE(float)
DEFINE_TYPE(GField)

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
  if (type.is<Field<float>>()) {
    return &SocketValueVariant2::init_default<Field<float>>(value);
  }
  return nullptr;
}

void *SocketValueVariant2::allocate(const CPPType &type, detail::SocketValueVariantAny &value)
{
  if (type.is<int>()) {
    return value.allocate<int>();
  }
  if (type.is<float>()) {
    return value.allocate<float>();
  }
  if (type.is<GField>()) {
    return value.allocate<GField>();
  }
  BLI_assert_unreachable();
  return nullptr;
}

}  // namespace blender::bke
