/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_behaviors.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "BKE_node_socket_value.hh"

namespace blender::nodes {

BehaviorDef &BehaviorListDef::add(std::string name)
{
  auto def_ptr = std::make_shared<BehaviorDef>();
  BehaviorDef &def = *def_ptr;
  this->behaviors.add_new(std::move(def_ptr));

  def.type = name;
  return def;
}

void BehaviorRegistry::add(std::shared_ptr<const BehaviorListDef> behavior_list_def)
{
  behavior_list_defs_.add(std::move(behavior_list_def));
}

VectorSet<std::string> BehaviorRegistry::get_all_behavior_names() const
{
  VectorSet<std::string> names;
  for (const std::shared_ptr<const BehaviorListDef> &behavior_list_def : behavior_list_defs_) {
    for (const std::shared_ptr<BehaviorDef> &behavior_def : behavior_list_def->behaviors) {
      names.add(behavior_def->type);
    }
  }
  return names;
}

BehaviorRegistry &get_behavior_registry()
{
  static BehaviorRegistry registry;
  return registry;
}

}  // namespace blender::nodes

namespace blender::nodes::behaviors {

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

bool behavior_path_is_selected(StringRef self_path, StringRef filter, StringRef other)
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

}  // namespace blender::nodes::behaviors
