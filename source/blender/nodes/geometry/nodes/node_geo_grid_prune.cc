

/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_process.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

#include "openvdb/tools/Prune.h"

namespace blender::nodes::node_geo_grid_prune_cc {

// enum class Mode : int16_t {
//   Inactive = 0,
//   Threshold = 1,
//   SDF = 2,
// };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);
  b.add_output(data_type, "Grid").structure_type(StructureType::Grid).align_with_previous();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  // layout->prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static std::optional<eNodeSocketDatatype> node_type_for_socket_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return SOCK_FLOAT;
    case SOCK_BOOLEAN:
      return SOCK_BOOLEAN;
    case SOCK_INT:
      return SOCK_INT;
    case SOCK_VECTOR:
    case SOCK_RGBA:
      return SOCK_VECTOR;
    default:
      return std::nullopt;
  }
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  if (!USER_EXPERIMENTAL_TEST(&U, use_new_volume_nodes)) {
    return;
  }
  const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(
      params.other_socket());
  if (!data_type) {
    return;
  }
  params.add_item(IFACE_("Grid"), [data_type](LinkSearchOpParams &params) {
    bNode &node = params.add_node("GeometryNodeGridPrune");
    node.custom1 = *data_type;
    params.update_and_connect_available_socket(node, "Grid");
  });
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  // const Mode mode = Mode(params.node().custom2);
  bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid");
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }
  bke::VolumeTreeAccessToken tree_token;
  openvdb::GridBase &grid_base = grid.get_for_write().grid_for_write(tree_token);
  // switch (mode) {
  //   case Mode::Inactive:
  bke::volume_grid::to_typed_grid(grid_base,
                                  [&](auto &grid) { openvdb::tools::pruneInactive(grid.tree()); });
  //     break;
  //   case Mode::Threshold:
  //     break;
  //   case Mode::SDF: {
  //     const VolumeGridType grid_type = bke::volume_grid::get_type(grid_base);
  //     BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
  //       using GridT = typename decltype(type_tag)::type;
  //       if constexpr (bke::volume_grid::is_supported_grid_type<GridT>) {
  //         if constexpr (std::is_scalar_v<typename GridT::ValueType>) {
  //         }
  //       }
  //       else {
  //         BLI_assert_unreachable();
  //       }
  //     });

  //     bke::volume_grid::to_typed_grid(
  //         vdb_grid, [&](auto &grid) { openvdb::tools::pruneLevelSet(grid.tree()); });
  //     break;
  //   }
  // }
  params.set_output("Grid", std::move(grid));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
  // node->custom2 = int16_t(Mode::Inactive);
}

static void node_rna(StructRNA *srna)
{
  // EnumPropertyItem mode_items[] = {
  //     {int(Mode::Inactive), "INACTIVE", 0, "Inactive", ""},
  //     {int(Mode::Threshold), "THRESHOLD", 0, "Threshold", ""},
  //     {int(Mode::SDF), "SDF", 0, "SDF", ""},
  //     {0, nullptr, 0, nullptr, nullptr},
  // };
  // RNA_def_node_enum(srna,
  //                   "mode",
  //                   "Mode",
  //                   "",
  //                   mode_items,
  //                   NOD_inline_enum_accessors(custom1),
  //                   int(Mode::Inactive));
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Node socket data type",
                    rna_enum_node_socket_data_type_items,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeGridPrune");
  ntype.ui_name = "Prune Grid";
  ntype.ui_description = "Make inactive voxels and tiles tiles";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_prune_cc
