/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_nodes_bundle_fwd.hh"
#include "BKE_node.hh"

#include "BLI_cpp_type.hh"
#include "BLI_generic_pointer.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_math_base.h"

#include "DNA_node_types.h"

namespace blender::bke {

class SocketInterfaceKey {
 private:
  /** May have multiple keys to improve compatibility between systems that use different keys. */
  Vector<std::string> identifiers_;

 public:
  explicit SocketInterfaceKey(std::string identifier);

  bool matches(const SocketInterfaceKey &other) const;
  Span<std::string> identifiers() const;
};

class Bundle : public ImplicitSharingMixin {
 private:
  struct Item {
    SocketInterfaceKey key;
    GMutablePointer value;
  };
  Vector<Item> items_;
  Vector<void *> buffers_;

 public:
  Bundle();
  Bundle(const Bundle &other);
  Bundle(Bundle &&other) noexcept;
  Bundle &operator=(const Bundle &other);
  Bundle &operator=(Bundle &&other) noexcept;
  ~Bundle();

  static BundlePtr create()
  {
    return BundlePtr(MEM_new<Bundle>(__func__));
  }

  void add_new(SocketInterfaceKey key, const CPPType &type, const void *value);
  bool add(const SocketInterfaceKey &key, const CPPType &type, const void *value);
  bool add(SocketInterfaceKey &&key, const CPPType &type, const void *value);
  GPointer lookup(const SocketInterfaceKey &key) const;
  GMutablePointer lookup_for_write(const SocketInterfaceKey &key);
  bool remove(const SocketInterfaceKey &key);
  bool contains(const SocketInterfaceKey &key) const;

  void delete_self() override;
};

class SocketListSignature {
 public:
  struct Item {
    const bNodeSocketType *socket_type = nullptr;
    std::string name;
  };

  Vector<Item> items;

  static const std::shared_ptr<SocketListSignature> &empty()
  {
    static const std::shared_ptr<SocketListSignature> empty_signature =
        std::make_shared<SocketListSignature>();
    return empty_signature;
  }
};

inline void get_socket_list_signature_map(const SocketListSignature &signature_a,
                                          const SocketListSignature &signature_b,
                                          MutableSpan<std::optional<int>> r_a_by_b)
{
  BLI_assert(r_a_by_b.size() == signature_b.items.size());

  for (const int a_i : signature_a.items.index_range()) {
    const SocketListSignature::Item &item_a = signature_a.items[a_i];
    for (const int b_i : signature_b.items.index_range()) {
      const SocketListSignature::Item &item_b = signature_b.items[b_i];
      if (item_a.name == item_b.name && item_a.socket_type == item_b.socket_type) {
        r_a_by_b[b_i] = a_i;
      }
    }
  }
}

}  // namespace blender::bke
