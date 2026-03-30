/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/Grid.h>
#endif

namespace blender::nodes::node_geo_grid_mask_topology {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bool>("Grid").hide_value().structure_type(StructureType::Grid);
  b.add_output<decl::Bool>("Grid").structure_type(StructureType::Grid).align_with_previous();
}

#ifdef WITH_OPENVDB

template<typename TreeT> struct BoolGridTopologyOp {
 public:
  using RootT = typename TreeT::RootNodeType;
  using LeafT = typename TreeT::LeafNodeType;
  using ValueT = typename TreeT::ValueType;

  bool operator()(RootT &root, size_t /*node_index*/) const
  {
    for (auto it = root.beginValueOn(); it; ++it) {

      if (!(*it)) {
        it.setValueOn(false);
      }
    }
    return true;
  }

  template<typename NodeT> bool operator()(NodeT &node, size_t /*node_index*/) const
  {
    /* Only iterate if there are active tiles. */
    if (!node.isValueMaskOff()) {
      for (auto it = node.beginValueOn(); it; ++it) {
        if (!(*it)) {
          it.setValueOn(false);
        }
      }
    }
    /* Return false if there are no child nodes below this node. */
    return !node.isChildMaskOff();
  }

  bool operator()(LeafT &leaf, size_t /*node_index*/) const
  {
    /* Early-exit if there are no active values. */
    if (leaf.isValueMaskOff()) {
      return true;
    }
    for (auto it = leaf.beginValueOn(); it; ++it) {
      if (!(*it)) {
        it.setValueOn(false);
      }
    }
    return true;
  }
};

template<typename GridOrTree> void bool_grid_topology(GridOrTree &gridOrTree, const bool threaded)
{
  using Adapter = openvdb::TreeAdapter<GridOrTree>;
  using TreeType = typename Adapter::TreeType;

  TreeType &tree = Adapter::tree(gridOrTree);

  openvdb::tree::DynamicNodeManager<TreeType> nodeManager(tree);

  BoolGridTopologyOp<TreeType> op;
  nodeManager.foreachTopDown(op, threaded);
}

#endif

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  bke::VolumeGrid<bool> grid = params.extract_input<bke::VolumeGrid<bool>>("Grid"_ustr);
  if (grid) {
    bke::VolumeTreeAccessToken grid_access_token;
    openvdb::BoolGrid &vdb_grid = grid.grid_for_write(grid_access_token);
    bool_grid_topology(vdb_grid, true);
    grid->tag_tree_modified();
  }

  params.set_output("Grid"_ustr, std::move(grid));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGridMaskTopology");
  ntype.ui_name = "Grid Mask Topology";
  ntype.ui_description = "Deactivate voxels of a boolean grid based on value";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_mask_topology
