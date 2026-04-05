/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_any.hh"
#include "BLI_cpp_type.hh"
#include "BLI_generic_pointer.hh"
#include "BLI_memory_counter_fwd.hh"

#include "BKE_node_socket_value_fwd.hh"

namespace blender::bke {

namespace detail {

enum class Kind {
  None,
  Single,
  Field,
  Grid,
  List,
};

struct SocketValueVariantTypeInfo {
  Kind kind;
  const CPPType &type;
  void (*convert_to)(const CPPType &type, SocketValueVariant &value);

  template<typename T> static SocketValueVariantTypeInfo get();
};

template<typename T>
concept has_generic_type = requires { typename T::generic_type; };
template<typename T> struct storage_type {
  using type = T;
};
template<has_generic_type T> struct storage_type<T> {
  using type = typename T::generic_type;
};

};  // namespace detail

class SocketValueVariant2 {
 private:
  Any<detail::SocketValueVariantTypeInfo, 32, 16> value_;

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
    requires(std::is_trivial_v<std::decay_t<T>> || is_same_any_v<std::decay_t<T>, std::string>)
  {
    this->emplace<std::decay_t<T>>(std::forward<T>(value));
  }

  template<typename T, typename... Args> T &emplace(Args &&...args)
  {
    return value_.emplace<to_storage_type<T>>(T(std::forward<Args>(args)...));
  }

  template<typename T> T &ensure_type();
  void *ensure_type(const CPPType &type);

  template<typename T> const T *get_if() const;
  template<typename T> T *get_if();

  GPointer get() const;
  GMutablePointer get();

  void *allocate_single(const CPPType &type);

  void count_memory(MemoryCounter &memory) const;
};

}  // namespace blender::bke
