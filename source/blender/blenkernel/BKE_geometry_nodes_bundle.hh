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

/**
 * A key that identifies values in a bundle or inputs/outputs of a closure.
 * Note that this key does not have a hash and thus can't be used in a hash table. This wouldn't
 * work well if these items have multiple identifiers for compatibility reasons. While that's not
 * used currently, it's good to keep it possible.
 */
class SocketInterfaceKey {
 private:
  /** May have multiple keys to improve compatibility between systems that use different keys. */
  Vector<std::string> identifiers_;

 public:
  explicit SocketInterfaceKey(Vector<std::string> identifiers);
  explicit SocketInterfaceKey(std::string identifier);

  bool matches(const SocketInterfaceKey &other) const;
  Span<std::string> identifiers() const;
};

/**
 * A bundle is a map containing keys and their corresponding values. Values are stored as the type
 * they have in Geometry Nodes (#bNodeSocketType::geometry_nodes_cpp_type).
 */
class Bundle : public ImplicitSharingMixin {
 public:
  struct StoredItem {
    SocketInterfaceKey key;
    const bNodeSocketType *type;
    void *value;
  };

 private:
  Vector<StoredItem> items_;
  Vector<void *> buffers_;

 public:
  struct Item {
    const bNodeSocketType *type;
    const void *value;
  };

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

  void add_new(SocketInterfaceKey key, const bNodeSocketType &type, const void *value);
  bool add(const SocketInterfaceKey &key, const bNodeSocketType &type, const void *value);
  bool add(SocketInterfaceKey &&key, const bNodeSocketType &type, const void *value);
  bool remove(const SocketInterfaceKey &key);
  bool contains(const SocketInterfaceKey &key) const;

  std::optional<Item> lookup(const SocketInterfaceKey &key) const;

  Span<StoredItem> items() const
  {
    return items_;
  }

  void delete_self() override;
};

}  // namespace blender::bke
