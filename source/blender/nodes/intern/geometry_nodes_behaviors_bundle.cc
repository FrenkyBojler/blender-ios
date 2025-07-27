/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_behaviors_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "BKE_node_socket_value.hh"

namespace blender::nodes {

static void foreach_behavior_recursive(
    const Bundle &behaviors_bundle,
    Vector<StringRef> &path_stack,
    const FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)>
        fn)
{
  if (const std::optional<std::string> type = behaviors_bundle.lookup<std::string>("Type")) {
    if (type->empty()) {
      return;
    }
    fn(*type, behaviors_bundle, path_stack);
    return;
  }
  for (const Bundle::StoredItem &item : behaviors_bundle.items()) {
    const BundleItemSocketValue *socket_value = std::get_if<BundleItemSocketValue>(
        &item.value.value);
    if (!socket_value) {
      continue;
    }
    if (socket_value->type->type != SOCK_BUNDLE) {
      continue;
    }
    BundlePtr child_bundle =
        static_cast<bke::SocketValueVariant *>(socket_value->value)->get<BundlePtr>();
    if (!child_bundle) {
      continue;
    }
    path_stack.append(item.key);
    foreach_behavior_recursive(*child_bundle, path_stack, fn);
    path_stack.pop_last();
  }
}

void foreach_behavior_in_bundle(
    const Bundle &behaviors_bundle,
    const FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)>
        fn)
{
  Vector<StringRef> path_stack;
  foreach_behavior_recursive(behaviors_bundle, path_stack, fn);
}

}  // namespace blender::nodes
