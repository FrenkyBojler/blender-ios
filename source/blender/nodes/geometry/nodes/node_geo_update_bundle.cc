/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "BKE_compute_contexts.hh"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_closure_eval.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_update_bundle_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").pass_through_input_index(0).align_with_previous();
  b.add_input<decl::String>("Closure Path")
      .optional_label()
      .description(
          "If a (nested) bundle has a closure with at that path, it will be evaluated at that "
          "level to update the bundle");
  b.add_input<decl::Closure>("Closure").description(
      "Closure that is evaluated at every level of the bundle");
  b.add_input<decl::Bundle>("Reference");
}

class BundleUpdater {
 private:
  const bNode &node_;
  const std::optional<Vector<StringRef>> &closure_path_;
  ClosurePtr global_closure_ptr_;
  GeoNodesUserData &geo_user_data_;

 public:
  BundleUpdater(const bNode &node,
                const std::optional<Vector<StringRef>> &closure_path,
                ClosurePtr global_closure,
                GeoNodesUserData &geo_user_data)
      : node_(node),
        closure_path_(closure_path),
        global_closure_ptr_(std::move(global_closure)),
        geo_user_data_(geo_user_data)
  {
  }

  BundlePtr update(const std::string bundle_path,
                   BundlePtr main_bundle_ptr,
                   BundlePtr reference_bundle_ptr)
  {
    if (!main_bundle_ptr) {
      return {};
    }
    if (main_bundle_ptr->is_empty()) {
      return main_bundle_ptr;
    }
    /* Remember old keys so that we don't recurse into newly inserted nested bundles. */
    Vector<std::string> old_keys;
    for (const auto &item : main_bundle_ptr->items()) {
      old_keys.append(item.key);
    }
    if (global_closure_ptr_) {
      bke::UpdateBundleComputeContext compute_context{
          geo_user_data_.compute_context, node_.identifier, true, bundle_path};
      main_bundle_ptr = this->update_bundle_with_closure(
          std::move(main_bundle_ptr), reference_bundle_ptr, *global_closure_ptr_, compute_context);
    }
    if (closure_path_) {
      const std::optional<ClosurePtr> update_closure_ptr =
          main_bundle_ptr->lookup_path<ClosurePtr>(*closure_path_);
      if (update_closure_ptr && *update_closure_ptr) {
        const Closure &update_closure = **update_closure_ptr;
        bke::UpdateBundleComputeContext compute_context{
            geo_user_data_.compute_context, node_.identifier, false, bundle_path};
        main_bundle_ptr = this->update_bundle_with_closure(
            std::move(main_bundle_ptr), reference_bundle_ptr, update_closure, compute_context);
      }
    }
    if (!main_bundle_ptr) {
      return {};
    }
    if (!this->has_update_callback_recursive(*main_bundle_ptr)) {
      return main_bundle_ptr;
    }

    Bundle &main_bundle = main_bundle_ptr.ensure_mutable_inplace();
    for (const StringRef key : old_keys) {
      BundleItemValue *value = main_bundle.lookup(key);
      if (!value) {
        continue;
      }
      BundlePtr *child_bundle_ptr = value->as_pointer<BundlePtr>();
      if (!child_bundle_ptr || !*child_bundle_ptr) {
        /* Keep the value as is.*/
        continue;
      }
      BundlePtr reference_child;
      if (reference_bundle_ptr) {
        if (const BundlePtr *reference_child_ptr = reference_bundle_ptr->lookup_ptr<BundlePtr>(
                key))
        {
          reference_child = *reference_child_ptr;
        }
      }
      *child_bundle_ptr = this->update(
          fmt::format("{}{}/", bundle_path, key), *child_bundle_ptr, reference_child);
    }
    return main_bundle_ptr;
  }

  bool has_update_callback_recursive(const Bundle &bundle) const
  {
    if (closure_path_) {
      const ClosurePtr *closure = bundle.lookup_path_ptr<ClosurePtr>(*closure_path_);
      if (closure && *closure) {
        return true;
      }
    }
    for (const auto &item : bundle.items()) {
      const BundlePtr *child_bundle_ptr = item.value.as_pointer<BundlePtr>();
      if (!child_bundle_ptr || !*child_bundle_ptr) {
        continue;
      }
      if (global_closure_ptr_) {
        return true;
      }
      if (this->has_update_callback_recursive(**child_bundle_ptr)) {
        return true;
      }
    }
    return false;
  }

  BundlePtr update_bundle_with_closure(BundlePtr main_bundle,
                                       BundlePtr reference_bundle,
                                       const Closure &closure,
                                       const ComputeContext &compute_context)
  {
    GeoNodesUserData sub_geo_user_data = geo_user_data_;
    sub_geo_user_data.compute_context = &compute_context;
    sub_geo_user_data.log_socket_values = should_log_socket_values_for_context(
        geo_user_data_, compute_context.hash());

    ClosureEagerEvalParams closure_eval_params;
    closure_eval_params.user_data = &sub_geo_user_data;
    static const bke::bNodeSocketType &bundle_socket_type = *bke::node_socket_type_find_static(
        SOCK_BUNDLE);
    closure_eval_params.inputs.append(
        {"Value", &bundle_socket_type, SocketValueVariant::From(std::move(main_bundle))});
    closure_eval_params.inputs.append(
        {"Reference", &bundle_socket_type, SocketValueVariant::From(std::move(reference_bundle))});
    SocketValueVariant output_value;
    std::destroy_at(&output_value);
    closure_eval_params.outputs.append({"Value", &bundle_socket_type, &output_value});
    evaluate_closure_eagerly(closure, closure_eval_params);
    return output_value.extract<BundlePtr>();
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr main = params.extract_input<BundlePtr>("Bundle");
  if (!main) {
    params.set_default_remaining_outputs();
    return;
  }
  const std::string closure_path = params.extract_input<std::string>("Closure Path");
  const std::optional<Vector<StringRef>> parsed_closure_path = Bundle::split_path(closure_path);

  const ClosurePtr global_closure = params.extract_input<ClosurePtr>("Closure");
  BundlePtr reference = params.extract_input<BundlePtr>("Reference");

  BundleUpdater updater(params.node(), parsed_closure_path, global_closure, *params.user_data());
  BundlePtr updated = updater.update("", std::move(main), std::move(reference));
  params.set_output("Bundle", std::move(updated));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeUpdateBundle");
  ntype.ui_name = "Update Bundle";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_update_bundle_cc
