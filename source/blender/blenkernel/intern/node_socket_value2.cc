/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"
#include "BKE_type_conversions.hh"

#include "FN_field.hh"
#include "FN_field_evaluation.hh"

#include "BKE_volume_grid.hh"

#include "BKE_volume_grid_multi_function_eval.hh"

#include "NOD_geometry_nodes_list.hh"

namespace blender::bke {

using fn::Field;
using fn::GField;
using nodes::GList;
using nodes::List;
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
#ifdef WITH_OPENVDB
  else if constexpr (std::is_same_v<CurrentT, GVolumeGrid>) {
    GVolumeGrid &src_grid = value.get<GVolumeGrid>();
    if (dst_type.generic_type && dst_type.generic_type->is<GVolumeGrid>()) {
      if (!src_grid) {
        /* Nothing to do. */
        return;
      }
      const CPPType *src_base_type = src_grid->cpp_type();
      if (!src_base_type) {
        /* Unknown type. */
        SocketValueVariant2::init_default(dst_type, value);
        return;
      }
      if (src_base_type == dst_type.base_type) {
        /* Nothing to do. */
        return;
      }
      const ConversionFunctions *fns = conversions.get_conversion_functions(*src_base_type,
                                                                            *dst_type.base_type);
      if (!fns || !fns->multi_function) {
        SocketValueVariant2::init_default(dst_type, value);
        return;
      }
      VolumeTreeAccessToken tree_token;
      const openvdb::GridBase &src_grid_base = src_grid->grid(tree_token);
      using namespace volume_grid::multi_function_eval;
      EvalResult conversion_result = evaluate_multi_function_on_grid(
          *fns->multi_function, {&src_grid_base}, {true});
      if (std::holds_alternative<EvalResult::Failure>(conversion_result.result)) {
        SocketValueVariant2::init_default(dst_type, value);
        return;
      }
      src_grid = GVolumeGrid(
          std::move(std::get<EvalResult::Success>(conversion_result.result).output_grids[0]));
      return;
    }
    SocketValueVariant2::init_default(dst_type, value);
    return;
  }
#endif
  else if constexpr (std::is_same_v<CurrentT, GList>) {
    // TODO
    SocketValueVariant2::init_default(dst_type, value);
    return;
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
  if constexpr (std::is_same_v<CurrentT, GField>) {
    if (dst_type.is<GField>()) {
      return true;
    }
    if (!dst_type.generic_type) {
      return false;
    }
    if (dst_type.generic_type->is<GField>()) {
      const GField &field = value.get<GField>();
      const CPPType &base_type = field.cpp_type();
      return base_type == *dst_type.base_type;
    }
    return false;
  }
#ifdef WITH_OPENVDB
  else if constexpr (std::is_same_v<CurrentT, GVolumeGrid>) {
    if (dst_type.is<GVolumeGrid>()) {
      return true;
    }
    if (!dst_type.generic_type) {
      return false;
    }
    if (dst_type.generic_type->is<GVolumeGrid>()) {
      const GVolumeGrid &grid = value.get<GVolumeGrid>();
      if (!grid) {
        return true;
      }
      const CPPType *grid_value_type = grid->cpp_type();
      return grid_value_type == dst_type.base_type;
    }
    return false;
  }
#endif
  else if constexpr (std::is_same_v<CurrentT, GList>) {
    return dst_type.is<GList>();
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

DEFINE_TYPE(int)
DEFINE_TYPE(float)
DEFINE_TYPE(GField)

#ifdef WITH_OPENVDB
DEFINE_TYPE(volume_grid::GVolumeGrid)
#endif

}  // namespace detail

template<typename T>
inline T &SocketValueVariant2::init_default(detail::SocketValueVariantAny &value)
{
  if constexpr (requires { typename T::generic_type; }) {
    using GenericType = typename T::generic_type;
    using BaseType = typename T::base_type;
    if constexpr (std::is_same_v<GenericType, GField>) {
      const CPPType &base_cpp_type = CPPType::get<BaseType>();
      return value.emplace<GField>(base_cpp_type).typed<BaseType>();
    }
#ifdef WITH_OPENVDB
    else if constexpr (std::is_same_v<GenericType, GVolumeGrid>) {
      return value.emplace<GVolumeGrid>(GVolumeGrid{}).typed<BaseType>();
    }
#endif
  }
  else if constexpr (std::is_same_v<T, GField>) {
    /* Some default fallback type. */
    return value.emplace<GField>(CPPType::get<float>());
  }
#ifdef WITH_OPENVDB
  else if constexpr (std::is_same_v<T, GVolumeGrid>) {
    return value.emplace<GVolumeGrid>();
  }
#endif
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
#ifdef WITH_OPENVDB
  if (type.is<GVolumeGrid>()) {
    return &SocketValueVariant2::init_default<GVolumeGrid>(value);
  }
  if (type.is<VolumeGrid<float>>()) {
    return &SocketValueVariant2::init_default<VolumeGrid<float>>(value);
  }
  if (type.is<VolumeGrid<int>>()) {
    return &SocketValueVariant2::init_default<VolumeGrid<int>>(value);
  }
#endif
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
#ifdef WITH_OPENVDB
  if (type.is<GVolumeGrid>()) {
    return value.allocate<GVolumeGrid>();
  }
#endif
  BLI_assert_unreachable();
  return nullptr;
}

}  // namespace blender::bke
