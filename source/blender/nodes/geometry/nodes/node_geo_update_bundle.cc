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
  b.add_input<decl::String>("Closure Name").optional_label();
  b.add_input<decl::Bundle>("Reference");
}

class BundleUpdater {
 private:
  const bNode &node_;
  std::string closure_name_;
  GeoNodesUserData &geo_user_data_;

 public:
  BundleUpdater(const bNode &node, std::string closure_name, GeoNodesUserData &geo_user_data)
      : node_(node), closure_name_(std::move(closure_name)), geo_user_data_(geo_user_data)
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
    const std::optional<ClosurePtr> update_closure_ptr = main_bundle_ptr->lookup<ClosurePtr>(
        closure_name_);
    if (update_closure_ptr && *update_closure_ptr) {
      const Closure &update_closure = **update_closure_ptr;

      bke::UpdateBundleComputeContext compute_context{
          geo_user_data_.compute_context, node_.identifier, bundle_path};
      GeoNodesUserData sub_geo_user_data = geo_user_data_;
      sub_geo_user_data.compute_context = &compute_context;
      sub_geo_user_data.log_socket_values = should_log_socket_values_for_context(
          geo_user_data_, compute_context.hash());

      ClosureEagerEvalParams closure_eval_params;
      closure_eval_params.user_data = &sub_geo_user_data;
      static const bke::bNodeSocketType &bundle_socket_type = *bke::node_socket_type_find_static(
          SOCK_BUNDLE);
      closure_eval_params.inputs.append(
          {"Value", &bundle_socket_type, SocketValueVariant::From(std::move(main_bundle_ptr))});
      closure_eval_params.inputs.append(
          {"Reference",
           &bundle_socket_type,
           SocketValueVariant::From(std::move(reference_bundle_ptr))});
      SocketValueVariant output_value;
      std::destroy_at(&output_value);
      closure_eval_params.outputs.append({"Value", &bundle_socket_type, &output_value});
      evaluate_closure_eagerly(update_closure, closure_eval_params);
      main_bundle_ptr = output_value.extract<BundlePtr>();
    }
    if (!main_bundle_ptr) {
      return {};
    }
    if (!this->has_update_callback_recursive(*main_bundle_ptr)) {
      return main_bundle_ptr;
    }

    Bundle &main_bundle = main_bundle_ptr.ensure_mutable_inplace();
    for (auto item : main_bundle.items()) {
      const StringRef key = item.key;
      BundleItemValue &value = item.value;
      BundlePtr *child_bundle_ptr = value.as_pointer<BundlePtr>();
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
    const ClosurePtr *closure = bundle.lookup_ptr<ClosurePtr>(closure_name_);
    if (closure && *closure) {
      return true;
    }
    for (const auto &item : bundle.items()) {
      const BundlePtr *child_bundle_ptr = item.value.as_pointer<BundlePtr>();
      if (!child_bundle_ptr || !*child_bundle_ptr) {
        continue;
      }
      if (this->has_update_callback_recursive(**child_bundle_ptr)) {
        return true;
      }
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr main = params.extract_input<BundlePtr>("Bundle");
  if (!main) {
    params.set_default_remaining_outputs();
    return;
  }
  const std::string closure_name = params.extract_input<std::string>("Closure Name");
  if (closure_name.empty()) {
    params.set_output("Bundle", std::move(main));
    return;
  }
  BundlePtr reference = params.extract_input<BundlePtr>("Reference");

  BundleUpdater updater(params.node(), closure_name, *params.user_data());
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
