/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.h"

#include "BLI_array_utils.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_apply_simulated_data_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("World");
  b.add_output<decl::Bundle>("World").pass_through_input_index(0).align_with_previous();
  b.add_input<decl::Bundle>("Simulated Data");
}

class SimDataApplier {
 private:
  Bundle &root_world_bundle_;
  const Bundle &root_sim_data_bundle_;

 public:
  SimDataApplier(Bundle &root_world_bundle, const Bundle &root_sim_data_bundle)
      : root_world_bundle_(root_world_bundle), root_sim_data_bundle_(root_sim_data_bundle)
  {
  }

  void apply()
  {
    this->apply_bundle(root_world_bundle_, root_sim_data_bundle_);
  }

  void apply_bundle(Bundle &world, const Bundle &sim_data)
  {
    const std::optional<std::string> type = world.lookup<std::string>(Bundle::type_item_name);
    if (!type) {
      this->apply_bundle_items(world, sim_data);
      return;
    }
    if (type == physics_bundles::XPBDGeometryBundle::name) {
      this->apply_xpbd_geometry_bundle(world, sim_data);
      return;
    }
    if (type == "blender.XpbdSolverState") {
      world.clear();
      for (auto item : sim_data.items()) {
        world.add(item.key, item.value);
      }
      return;
    }
  }

  void apply_bundle_items(Bundle &world, const Bundle &sim_data)
  {
    for (auto item : world.items()) {
      const StringRef key = item.key;
      BundleItemValue &world_value = item.value;
      const BundleItemValue *sim_data_value = sim_data.lookup(key);
      if (!sim_data_value) {
        continue;
      }
      BundlePtr *world_child_ptr = world_value.as_pointer<BundlePtr>();
      if (!world_child_ptr) {
        continue;
      }
      const BundlePtr *sim_data_child_ptr = sim_data_value->as_pointer<BundlePtr>();
      if (!sim_data_child_ptr || !*sim_data_child_ptr) {
        continue;
      }
      Bundle &world_child = world_child_ptr->ensure_mutable_inplace();
      const Bundle &sim_data_child = **sim_data_child_ptr;
      this->apply_bundle(world_child, sim_data_child);
    }
  }

  void apply_xpbd_geometry_bundle(Bundle &world, const Bundle &sim_data)
  {
    GeometrySet *world_geometry = world.lookup_ptr<GeometrySet>("geometry");
    const GeometrySet *sim_data_geometry = sim_data.lookup_ptr<GeometrySet>("geometry");
    if (!world_geometry || !sim_data_geometry) {
      return;
    }
    if (world_geometry->has_mesh() && sim_data_geometry->has_mesh()) {
      Mesh *world_mesh = world_geometry->get_mesh_for_write();
      const Mesh *sim_data_mesh = sim_data_geometry->get_mesh();
      if (world_mesh->verts_num == sim_data_mesh->verts_num) {
        world_mesh->vert_positions_for_write().copy_from(sim_data_mesh->vert_positions());
        world_mesh->tag_positions_changed();

        if (const bke::AttributeReader<float3> sim_velocities =
                sim_data_mesh->attributes().lookup<float3>("velocity", bke::AttrDomain::Point))
        {
          bke::SpanAttributeWriter<float3> world_velocities =
              world_mesh->attributes_for_write().lookup_or_add_for_write_span<float3>(
                  "velocity", bke::AttrDomain::Point);
          array_utils::copy(sim_velocities.varray, world_velocities.span);
          world_velocities.finish();
        }
      }
    }
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr world_bundle_ptr = params.extract_input<BundlePtr>("World");
  if (!world_bundle_ptr) {
    params.set_default_remaining_outputs();
    return;
  }
  BundlePtr simulated_data_bundle_ptr = params.extract_input<BundlePtr>("Simulated Data");
  if (!simulated_data_bundle_ptr) {
    params.set_output("World", std::move(world_bundle_ptr));
    return;
  }
  Bundle &world_bundle = world_bundle_ptr.ensure_mutable_inplace();
  SimDataApplier applier(world_bundle, *simulated_data_bundle_ptr);
  applier.apply();
  params.set_output("World", std::move(world_bundle_ptr));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeApplySimulatedData");
  ntype.ui_name = "Apply Sim Data";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_apply_simulated_data_cc
