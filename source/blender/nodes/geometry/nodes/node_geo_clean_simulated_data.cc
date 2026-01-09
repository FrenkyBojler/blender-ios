/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.h"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_clean_simulated_data_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Bundle>("Simulated Data");
  b.add_input<decl::Bundle>("World");
}

class SimDataCleaner {
 private:
  Bundle &root_bundle_;

 public:
  SimDataCleaner(Bundle &root_bundle) : root_bundle_(root_bundle) {}

  void clean()
  {
    this->clean_bundle(root_bundle_);
  }

  void clean_bundle(Bundle &bundle)
  {
    const std::optional<std::string> type = bundle.lookup<std::string>(Bundle::type_item_name);
    if (!type) {
      this->clean_bundle_items(bundle);
      return;
    }
    if (type == physics_bundles::XPBDGeometryBundle::name) {
      this->clean_xpbd_geometry_bundle(bundle);
      return;
    }
    if (type == "blender.XpbdSolverState") {
      return;
    }
    bundle.clear();
  }

  void clean_bundle_items(Bundle &bundle)
  {
    Vector<std::string> keys_to_remove;
    for (auto item : bundle.items()) {
      const StringRef key = item.key;
      BundleItemValue &value = item.value;
      if (BundlePtr *child_bundle_ptr = value.as_pointer<BundlePtr>()) {
        if (*child_bundle_ptr) {
          Bundle &child_bundle = child_bundle_ptr->ensure_mutable_inplace();
          this->clean_bundle(child_bundle);
          if (!child_bundle.is_empty()) {
            continue;
          }
        }
      }
      keys_to_remove.append(key);
    }
    for (const StringRef key : keys_to_remove) {
      bundle.remove(key);
    }
  }

  void clean_xpbd_geometry_bundle(Bundle &bundle)
  {
    Vector<std::string> keys_to_remove;
    for (auto item : bundle.items()) {
      const StringRef key = item.key;
      BundleItemValue &value = item.value;
      if (key == Bundle::type_item_name) {
        continue;
      }
      if (key == "geometry") {
        if (GeometrySet *geometry = value.as_pointer<GeometrySet>()) {
          this->clean_geometry(*geometry);
          continue;
        }
        keys_to_remove.append(key);
      }
      keys_to_remove.append(key);
    }
    for (const StringRef key : keys_to_remove) {
      bundle.remove(key);
    }
  }

  void clean_geometry(GeometrySet &main_geometry)
  {
    /* TODO: Referenced instance data could actually be fully removed. */
    /* TODO: Could also remove mesh topology data in many cases. */
    geometry::foreach_real_geometry(main_geometry, [&](GeometrySet &geometry) {
      if (geometry.has_mesh()) {
        Mesh *mesh = geometry.get_mesh_for_write();
        bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
        Vector<std::string> attributes_to_remove;
        attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
          if (BKE_mesh_attribute_required(iter.name)) {
            return;
          }
        });
        for (const StringRef attribute : attributes_to_remove) {
          attributes.remove(attribute);
        }
      }
    });
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr world_bundle_ptr = params.extract_input<BundlePtr>("World");
  if (!world_bundle_ptr) {
    params.set_default_remaining_outputs();
    return;
  }
  Bundle &bundle = world_bundle_ptr.ensure_mutable_inplace();
  SimDataCleaner cleaner(bundle);
  cleaner.clean();
  params.set_output("Simulated Data", std::move(world_bundle_ptr));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCleanSimulatedData");
  ntype.ui_name = "Clean Sim Data";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_clean_simulated_data_cc
