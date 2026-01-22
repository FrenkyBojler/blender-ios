/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#endif

namespace blender::nodes::node_geo_cube_grid_topology_cc {

enum class InputMode {
  Size = 0,
  Bounds = 1,
};

static const EnumPropertyItem input_mode_items[] = {
    {int(InputMode::Bounds), "BOUNDS", 0, N_("Bounds"), "Specify grid using minimum and maximum bounds in world space"},
    {int(InputMode::Size), "SIZE", 0, N_("Size"), "Specify the size of the grid in voxel space with no transform applied"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Menu>("Mode")
      .static_items(input_mode_items)
      .default_value(InputMode::Bounds)
      .expanded()
      .optional_label();

  b.add_input<decl::Int>("Min X")
      .default_value(0)
      .description("Minimum coordinate in X axis (grid index space)")
      .usage_by_single_menu(int(InputMode::Size));
  b.add_input<decl::Int>("Min Y")
      .default_value(0)
      .description("Minimum coordinate in Y axis (grid index space)")
      .usage_by_single_menu(int(InputMode::Size));
  b.add_input<decl::Int>("Min Z")
      .default_value(0)
      .description("Minimum coordinate in Z axis (grid index space)")
      .usage_by_single_menu(int(InputMode::Size));

  b.add_input<decl::Int>("Size X")
      .default_value(32)
      .min(1)
      .description("Size in X axis (number of voxels)")
      .usage_by_single_menu(int(InputMode::Size));
  b.add_input<decl::Int>("Size Y")
      .default_value(32)
      .min(1)
      .description("Size in Y axis (number of voxels)")
      .usage_by_single_menu(int(InputMode::Size));
  b.add_input<decl::Int>("Size Z")
      .default_value(32)
      .min(1)
      .description("Size in Z axis (number of voxels)")
      .usage_by_single_menu(int(InputMode::Size));

  b.add_input<decl::Vector>("Min")
      .default_value(float3(-1.0f))
      .description("Minimum boundary of the grid (world space)")
      .usage_by_single_menu(int(InputMode::Bounds));
  b.add_input<decl::Vector>("Max")
      .default_value(float3(1.0f))
      .description("Maximum boundary of the grid (world space)")
      .usage_by_single_menu(int(InputMode::Bounds));

  b.add_input<decl::Int>("Resolution X")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the X axis")
      .usage_by_single_menu(int(InputMode::Bounds));
  b.add_input<decl::Int>("Resolution Y")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the Y axis")
      .usage_by_single_menu(int(InputMode::Bounds));
  b.add_input<decl::Int>("Resolution Z")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the Z axis")
      .usage_by_single_menu(int(InputMode::Bounds));

  b.add_output<decl::Bool>("Topology")
      .structure_type(StructureType::Grid)
      .description("Boolean grid defining the topology/active regions");
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype other_type = eNodeSocketDatatype(params.other_socket().type);

  if (params.in_out() == SOCK_OUT) {
    if (params.node_tree().typeinfo->validate_link(SOCK_BOOLEAN, other_type)) {
      params.add_item(IFACE_("Topology"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        params.update_and_connect_available_socket(node, "Topology");
      });
    }
  }
  else {
    if (params.node_tree().typeinfo->validate_link(other_type, SOCK_INT)) {
      params.add_item(IFACE_("Min X"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        node.custom1 = int(InputMode::Size);
        params.update_and_connect_available_socket(node, "Min X");
      });
      params.add_item(IFACE_("Size X"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        node.custom1 = int(InputMode::Size);
        params.update_and_connect_available_socket(node, "Size X");
      });
      params.add_item(IFACE_("Resolution X"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        node.custom1 = int(InputMode::Bounds);
        params.update_and_connect_available_socket(node, "Resolution X");
      });
    }
    if (params.node_tree().typeinfo->validate_link(other_type, SOCK_VECTOR)) {
      params.add_item(IFACE_("Min"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        node.custom1 = int(InputMode::Bounds);
        params.update_and_connect_available_socket(node, "Min");
      });
      params.add_item(IFACE_("Max"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        node.custom1 = int(InputMode::Bounds);
        params.update_and_connect_available_socket(node, "Max");
      });
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const InputMode mode = params.get_input<InputMode>("Mode");
  using type_traits = typename bke::VolumeGridTraits<bool>;
  using TreeType = typename type_traits::TreeType;
  using GridType = openvdb::Grid<TreeType>;

  auto openvdb_grid = GridType::create(false /* background */);

  if (mode == InputMode::Size) {
    const int3 grid_min = int3(params.extract_input<int>("Min X"),
                               params.extract_input<int>("Min Y"),
                               params.extract_input<int>("Min Z"));

    const int3 grid_size = int3(params.extract_input<int>("Size X"),
                                params.extract_input<int>("Size Y"),
                                params.extract_input<int>("Size Z"));

    if (grid_size.x < 1 || grid_size.y < 1 || grid_size.z < 1) {
      params.error_message_add(NodeWarningType::Error, TIP_("Size must be greater than 0"));
      params.set_default_remaining_outputs();
      return;
    }

    const int3 grid_max = grid_min + grid_size - int3(1, 1, 1);
    openvdb::math::CoordBBox bbox({grid_min.x, grid_min.y, grid_min.z},
                                  {grid_max.x, grid_max.y, grid_max.z});
    openvdb_grid->tree().denseFill(bbox, true, /*active=*/true);
  }
  else if (mode == InputMode::Bounds) {
    const float3 bounds_min = params.extract_input<float3>("Min");
    const float3 bounds_max = params.extract_input<float3>("Max");

    const int3 resolution = int3(params.extract_input<int>("Resolution X"),
                                 params.extract_input<int>("Resolution Y"),
                                 params.extract_input<int>("Resolution Z"));

    if (resolution.x < 2 || resolution.y < 2 || resolution.z < 2) {
      params.error_message_add(NodeWarningType::Error, TIP_("Resolution must be greater than 1"));
      params.set_default_remaining_outputs();
      return;
    }

    if (bounds_min.x == bounds_max.x || bounds_min.y == bounds_max.y || bounds_min.z == bounds_max.z)
    {
      params.error_message_add(NodeWarningType::Error,
                               TIP_("Bounding box volume must be greater than 0"));
      params.set_default_remaining_outputs();
      return;
    }

    openvdb::math::CoordBBox bbox({0, 0, 0}, {resolution.x - 1, resolution.y - 1, resolution.z - 1});
    openvdb_grid->tree().denseFill(bbox, true, /*active=*/true);

    const double3 scale_fac = double3(bounds_max - bounds_min) / double3(resolution - 1);
    if (!BKE_volume_voxel_size_valid(float3(scale_fac))) {
      params.error_message_add(NodeWarningType::Warning,
                               TIP_("Volume scale is lower than permitted by OpenVDB"));
      params.set_default_remaining_outputs();
      return;
    }

    openvdb_grid->transform().postScale(openvdb::math::Vec3d(scale_fac.x, scale_fac.y, scale_fac.z));
    openvdb_grid->transform().postTranslate(
        openvdb::math::Vec3d(bounds_min.x, bounds_min.y, bounds_min.z));
  }

  bke::VolumeGrid<bool> topology_grid(std::move(openvdb_grid));
  params.set_output("Topology", bke::GVolumeGrid(std::move(topology_grid)));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}


static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCubeGridTopology");
  ntype.ui_name = "Cube Grid Topology";
  ntype.ui_description = "Initialize a boolean grid topology with given dimensions, for use with the Field to Grid node";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cube_grid_topology_cc
