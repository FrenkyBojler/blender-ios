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

namespace blender::nodes::node_geo_cube_grid_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Min")
      .default_value(float3(-1.0f))
      .description("Minimum boundary of the grid");
  b.add_input<decl::Vector>("Max")
      .default_value(float3(1.0f))
      .description("Maximum boundary of the grid");
  b.add_input<decl::Int>("Resolution X")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the X axis");
  b.add_input<decl::Int>("Resolution Y")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the Y axis");
  b.add_input<decl::Int>("Resolution Z")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the Z axis");
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
      params.add_item(IFACE_("Resolution X"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        params.update_and_connect_available_socket(node, "Resolution X");
      });
      params.add_item(IFACE_("Resolution Y"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        params.update_and_connect_available_socket(node, "Resolution Y");
      });
      params.add_item(IFACE_("Resolution Z"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        params.update_and_connect_available_socket(node, "Resolution Z");
      });
    }
    if (params.node_tree().typeinfo->validate_link(other_type, SOCK_VECTOR)) {
      params.add_item(IFACE_("Min"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        params.update_and_connect_available_socket(node, "Min");
      });
      params.add_item(IFACE_("Max"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeCubeGrid");
        params.update_and_connect_available_socket(node, "Max");
      });
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
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

  const double3 scale_fac = double3(bounds_max - bounds_min) / double3(resolution - 1);
  if (!BKE_volume_voxel_size_valid(float3(scale_fac))) {
    params.error_message_add(NodeWarningType::Warning,
                             TIP_("Volume scale is lower than permitted by OpenVDB"));
    params.set_default_remaining_outputs();
    return;
  }

  /* Create boolean grid directly in node_geo_exec. */
  using type_traits = typename bke::VolumeGridTraits<bool>;
  using TreeType = typename type_traits::TreeType;
  using GridType = openvdb::Grid<TreeType>;

  auto openvdb_grid = GridType::create(false /* background */);

  /* Fill all voxels in the cube densely using OpenVDB's denseFill. */
  openvdb::math::CoordBBox bbox({0, 0, 0}, {resolution.x - 1, resolution.y - 1, resolution.z - 1});
  openvdb_grid->tree().denseFill(bbox, true, /*active=*/true);

  /* Set transform from grid index space to world space. */
  openvdb_grid->transform().postScale(openvdb::math::Vec3d(scale_fac.x, scale_fac.y, scale_fac.z));
  openvdb_grid->transform().postTranslate(
      openvdb::math::Vec3d(bounds_min.x, bounds_min.y, bounds_min.z));

  bke::VolumeGrid<bool> topology_grid(std::move(openvdb_grid));
  params.set_output("Topology", bke::GVolumeGrid(std::move(topology_grid)));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCubeGrid");
  ntype.ui_name = "Cube Grid";
  ntype.ui_description = "Create a cube topology grid for use with Field to Grid node";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cube_grid_cc
