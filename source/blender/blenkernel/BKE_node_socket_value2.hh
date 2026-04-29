/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_any.hh"
#include "BLI_cpp_type.hh"
#include "BLI_generic_pointer.hh"
#include "BLI_memory_counter_fwd.hh"

#include "BKE_node_socket_value_fwd.hh"
#include <type_traits>
#include <utility>

namespace blender::fn {
class GField;
}
namespace blender::bke::volume_grid {
class GVolumeGrid;
}
namespace blender::nodes {
class List;
}

namespace blender::bke {

class SocketValueVariant2;

namespace detail {

struct SocketValueVariantTypeInfo;
using SocketValueVariantAny = Any<SocketValueVariantTypeInfo, 32, 16>;

struct SocketValueVariantTypeInfo {
  const CPPType &type;
  void (*convert_to)(const CPPType &dst_type, SocketValueVariantAny &value);
  bool (*is_interpretable_as)(const CPPType &dst_type, const SocketValueVariantAny &value);

  template<typename T> static SocketValueVariantTypeInfo get()
  {
    return SocketValueVariantTypeInfo{
        .type = *CPPType::get_pre_register<T>(),
        .convert_to = convert_to_fn<T>,
        .is_interpretable_as = is_interpretable_as_fn<T>,
    };
  }

  template<typename T>
  static void convert_to_fn(const CPPType &dst_type, SocketValueVariantAny &value);

  template<typename T>
  static bool is_interpretable_as_fn(const CPPType &dst_type, const SocketValueVariantAny &value);
};

template<typename T>
concept has_generic_type = requires { typename T::generic_type; };
template<typename T> struct storage_type {
  using type = T;
};
template<has_generic_type T> struct storage_type<T> {
  using type = typename T::generic_type;
};

inline const CPPType &to_storage_type(const CPPType &type)
{
  if (type.generic_type) {
    return *type.generic_type;
  }
  return type;  // NOLINT
}

};  // namespace detail

class SocketValueVariant2 {
 private:
  using Info = detail::SocketValueVariantTypeInfo;

  detail::SocketValueVariantAny value_;

  /**
   * Some types have a generic and compile-time version. For example, there is #GField and
   * #Field<T>. Those are expected to have the same memory layout so that references can be cast
   * between them. The #Any always stores the generic version if it exists.
   */
  template<typename T> using to_storage_type = typename detail::storage_type<T>::type;

 public:
  SocketValueVariant2() = default;

  template<typename T>
  explicit SocketValueVariant2(T &&value)
    requires(std::is_trivial_v<std::decay_t<T>> || is_same_any_v<std::decay_t<T>, std::string>);

  template<typename T, typename... Args> T &emplace(Args &&...args);

  template<typename T> T &ensure_type();
  void *ensure_type(const CPPType &type);

  template<typename T> const T *get_if() const;
  template<typename T> T *get_if();

  const void *get_if(const CPPType &type) const;
  void *get_if(const CPPType &type);

  GPointer get() const;
  GMutablePointer get();

  void *allocate_single(const CPPType &type);

  void count_memory(MemoryCounter &memory) const;

 private:
  template<typename T> T &init_default();
  void *init_default(const CPPType &type);

 public:
  template<typename T> static T &init_default(detail::SocketValueVariantAny &value);
  static void *init_default(const CPPType &type, detail::SocketValueVariantAny &value);
  static void *allocate(const CPPType &type, detail::SocketValueVariantAny &value);
};

template<typename T>
inline SocketValueVariant2::SocketValueVariant2(T &&value)
  requires(std::is_trivial_v<std::decay_t<T>> || is_same_any_v<std::decay_t<T>, std::string>)
{
  this->emplace<std::decay_t<T>>(std::forward<T>(value));
}

template<typename T, typename... Args> inline T &SocketValueVariant2::emplace(Args &&...args)
{
  using StorageT = to_storage_type<T>;
  StorageT &value = value_.emplace<StorageT>(T(std::forward<Args>(args)...));
  static_assert(sizeof(T) == sizeof(StorageT));
  return reinterpret_cast<T &>(value);
}

template<typename T> inline T &SocketValueVariant2::ensure_type()
{
  using StorageT = to_storage_type<T>;
  if (!value_.has_value()) {
    return this->init_default<T>();
  }
  const Info &info = value_.extra_info();
  const CPPType &requested_type = CPPType::get<T>();
  if (info.type == requested_type) {
    return reinterpret_cast<T &>(value_.get<StorageT>());
  }
  if (info.is_interpretable_as(requested_type, value_)) {
    return reinterpret_cast<T &>(value_.get<StorageT>());
  }
  info.convert_to(requested_type, value_);
  BLI_assert(value_.extra_info().is_interpretable_as(requested_type, value_));
  return reinterpret_cast<T &>(value_.get<StorageT>());
}

inline void *SocketValueVariant2::ensure_type(const CPPType &type)
{
  if (!value_.has_value()) {
    return this->init_default(type);
  }
  const Info &info = value_.extra_info();
  if (info.type == type) {
    return value_.get();
  }
  info.convert_to(type, value_);
  return value_.get();
}

template<typename T> inline const T *SocketValueVariant2::get_if() const
{
  if (!value_) {
    return nullptr;
  }
  const Info &info = value_.extra_info();
  const CPPType &requested_type = CPPType::get<T>();
  if (info.type == requested_type) {
    return &value_.get<T>();
  }
  if (info.is_interpretable_as(requested_type, value_)) {
    if constexpr (detail::has_generic_type<T>) {
      using GenericT = T::generic_type;
      return reinterpret_cast<const T *>(&value_.get<GenericT>());
    }
    else {
      return &value_.get<T>();
    }
  }
  return nullptr;
}

template<typename T> inline T *SocketValueVariant2::get_if()
{
  return const_cast<T *>(std::as_const(*this).get_if<T>());
}

inline const void *SocketValueVariant2::get_if(const CPPType &type) const
{
  if (!value_) {
    return nullptr;
  }
  const Info &info = value_.extra_info();
  if (info.type == type) {
    return value_.get();
  }
  if (info.is_interpretable_as(type, value_)) {
    return value_.get();
  }
  return nullptr;
}

inline void *SocketValueVariant2::get_if(const CPPType &type)
{
  return const_cast<void *>(std::as_const(*this).get_if(type));
}

inline GPointer SocketValueVariant2::get() const
{
  if (!value_) {
    return {};
  }
  const Info &info = value_.extra_info();
  return {info.type, value_.get()};
}

inline GMutablePointer SocketValueVariant2::get()
{
  if (!value_) {
    return {};
  }
  const Info &info = value_.extra_info();
  return {info.type, value_.get()};
}

template<typename T> inline T &SocketValueVariant2::init_default()
{
  return SocketValueVariant2::init_default<T>(value_);
}

inline void *SocketValueVariant2::init_default(const CPPType &type)
{
  return SocketValueVariant2::init_default(type, value_);
}

}  // namespace blender::bke
