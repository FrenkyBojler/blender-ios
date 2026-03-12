/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_process.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/tools/ValueTransformer.h>
#endif

namespace blender::nodes::node_geo_separate_grid {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);
  b.add_output(data_type, "Grid").structure_type(StructureType::Grid).align_with_previous();

  b.add_input<decl::Bool>("Mask Grid").structure_type(StructureType::Grid);
  b.add_input<decl::Bool>("Selection")
      .default_value(true)
      .hide_value()
      .field_on_all()
      .description("Voxels that go into the first output");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Node socket data type",
                    rna_enum_node_socket_data_type_items,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

#ifdef WITH_OPENVDB

static bool apply_mask_topology(const openvdb::GridBase &vdb_grid,
                                const openvdb::BoolGrid &vdb_mask_grid,
                                std::string &r_error_message)
{
  if (vdb_grid.transform() != vdb_mask_grid.transform()) {
    r_error_message = TIP_("Mask grid has incompatible transform");
    return false;
  }

  bke::volume_grid::to_typed_grid(
      vdb_grid, [&](auto &grid) { grid.topologyIntersection(vdb_mask_grid.tree()); });
  return true;
}

bke::GVolumeGrid separate_grid(bke::GVolumeGrid &grid,
                               const bke::VolumeGrid<bool> &mask_grid,
                               std::string &r_error_message)
{
  if (!grid) {
    return grid;
  }

  bke::VolumeTreeAccessToken grid_access_token;
  const openvdb::GridBase &vdb_grid = grid.get_for_write().grid_for_write(grid_access_token);
  const openvdb::math::Transform &transform = vdb_grid.transform();

  bke::VolumeTreeAccessToken mask_grid_access_token;
  const openvdb::BoolGrid *vdb_mask_grid;
  if (mask_grid) {
    vdb_mask_grid = &mask_grid.grid(mask_grid_access_token);
    if (vdb_grid.transform() != vdb_mask_grid->transform()) {
      r_error_message = TIP_("Mask grid has incompatible transform");
      vdb_mask_grid = nullptr;
    }
  }

  bke::volume_grid::to_typed_grid(vdb_grid, [&](auto &grid) {
    if (vdb_mask_grid) {
      grid.topologyIntersection(vdb_mask_grid->tree());
    }

    // bke::volume_grid::parallel_grid_topology_tasks(
    //     grid.tree,
    //     [&](const bke::volume_grid::LeafNodeMask &leaf_node_mask,
    //         const openvdb::CoordBBox &leaf_bbox,
    //         const bke::volume_grid::GetVoxelsFn get_voxels_fn) {
    //       process_leaf_node(fn,
    //                         input_values,
    //                         input_grids,
    //                         output_grids,
    //                         *transform,
    //                         leaf_node_mask,
    //                         leaf_bbox,
    //                         get_voxels_fn);
    //     },
    //     [&](const Span<openvdb::Coord> voxels) {
    //       process_voxels(fn, input_values, input_grids, output_grids, *transform, voxels);
    //     },
    //     [&](const Span<openvdb::CoordBBox> tiles) {
    //       process_tiles(fn, input_values, input_grids, output_grids, *transform, tiles);
    //     });

    // process_background(fn, input_values, input_grids, *transform, output_grids);
  });

  // Array<openvdb::GridBase::Ptr> output_grids(output_values.size());
  // for (const int i : output_values.index_range()) {
  //   if (!output_values[i]) {
  //     continue;
  //   }
  //   const int param_index = input_values.size() + i;
  //   const mf::ParamType param_type = fn.param_type(param_index);
  //   const CPPType &cpp_type = param_type.data_type().single_type();
  //   const std::optional<VolumeGridType> grid_type = cpp_type_to_grid_type(cpp_type);
  //   if (!grid_type) {
  //     r_error_message = TIP_("Grid type not supported");
  //     return false;
  //   }

  //   output_grids[i] = grid::create_grid_with_topology(mask_tree, *transform, *grid_type);
  // }
}

#endif

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid");
  bke::VolumeGrid<bool> mask_grid = params.extract_input<bke::VolumeGrid<bool>>("Mask Grid");

  std::string error_message;
  separate_grid(grid, mask_grid, error_message);

  params.set_output("Grid", std::move(grid));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSeparateGrid");
  ntype.ui_name = "Separate Grid";
  ntype.ui_description = "Separate grid voxels based on a mask grid and/or a filter condition";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_separate_grid
