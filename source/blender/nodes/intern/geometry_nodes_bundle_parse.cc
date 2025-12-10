/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

#include "BKE_node_socket_value.hh"

namespace blender::nodes {

static void nested_bundle_foreach_recursive(
    const Bundle &nested_bundle,
    Vector<StringRef> &path_stack,
    const FunctionRef<void(HandleNestedBundleParams &params)> fn)
{
  if (const std::optional<std::string> type = nested_bundle.lookup<std::string>(
          Bundle::type_item_name))
  {
    if (type->empty()) {
      return;
    }
    HandleNestedBundleParams params{*type, nested_bundle, path_stack};
    fn(params);
    return;
  }
  for (const auto &item : nested_bundle.items()) {
    const BundleItemSocketValue *socket_value = std::get_if<BundleItemSocketValue>(
        &item.value.value);
    if (!socket_value) {
      continue;
    }
    if (socket_value->type->type != SOCK_BUNDLE) {
      continue;
    }
    BundlePtr child_bundle = socket_value->value.get<BundlePtr>();
    if (!child_bundle) {
      continue;
    }
    path_stack.append(item.key);
    nested_bundle_foreach_recursive(*child_bundle, path_stack, fn);
    path_stack.pop_last();
  }
}

void nested_bundle_foreach(const Bundle &nested_bundle,
                           const FunctionRef<void(HandleNestedBundleParams &params)> fn)
{
  Vector<StringRef> path_stack;
  nested_bundle_foreach_recursive(nested_bundle, path_stack, fn);
}

bool nested_bundle_path_is_selected(StringRef self_path, StringRef filter, StringRef other)
{
  if (filter.is_empty()) {
    return true;
  }
  std::string absolute_filter;
  if (filter.startswith("./")) {
    const int sep = self_path.find_last_of('/');
    StringRef base_path;
    if (sep == StringRef::not_found) {
      base_path = "";
    }
    else {
      base_path = self_path.drop_suffix(self_path.size() - sep - 1);
    }
    absolute_filter = base_path + filter.drop_known_prefix("./");
  }
  else {
    absolute_filter = filter;
  }
  if (!other.startswith(absolute_filter)) {
    return false;
  }
  const StringRef remaining_other = other.drop_known_prefix(absolute_filter);
  return remaining_other.is_empty() || absolute_filter.empty() ||
         StringRef(absolute_filter).endswith("/") || remaining_other.startswith("/");
}

}  // namespace blender::nodes
