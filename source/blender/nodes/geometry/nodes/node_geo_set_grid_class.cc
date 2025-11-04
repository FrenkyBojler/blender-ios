/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_math_matrix.hh"

#include "BKE_attribute_math.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

namespace blender::nodes::node_geo_set_grid_class {

enum class GridClass : uint8_t {
  Unknown = 0,
  Density = 1,
  LevelSet = 2,
  Staggered = 3,
};

static const EnumPropertyItem grid_class_items[] = {
    {int(GridClass::Unknown), "UNKNOWN", 0, "Unknown", "Unspecified grid class"},
    {int(GridClass::Density), "DENSITY", 0, "Density", "Density or fog values"},
    {int(GridClass::LevelSet),
     "LEVEL_SET",
     0,
     "Level Set",
     "Level set or signed-distance field (SDF)"},
    {int(GridClass::Staggered),
     "STAGGERED",
     0,
     "Staggered",
     "Vector field with staggered component storage"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  b.add_input(data_type, "Grid")
      .hide_value()
      .structure_type(StructureType::Grid)
      .is_default_link_socket();
  b.add_output(data_type, "Grid").structure_type(StructureType::Grid).align_with_previous();
  b.add_input<decl::Menu>("Grid Class")
      .default_value(GridClass::Unknown)
      .static_items(grid_class_items)
      .optional_label();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
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
  const bNodeSocket &other_socket = params.other_socket();
  const StructureType structure_type = other_socket.runtime->inferred_structure_type;
  const bool is_grid = structure_type == StructureType::Grid;
  const bool is_dynamic = structure_type == StructureType::Dynamic;

  if (params.in_out() == SOCK_IN) {
    if (is_grid || is_dynamic) {
      const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(other_socket);
      if (data_type) {
        params.add_item(IFACE_("Grid"), [data_type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeSetGridClass");
          node.custom1 = *data_type;
          params.update_and_connect_available_socket(node, "Grid");
        });
      }
    }
  }
  else {
    const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(other_socket);
    if (data_type) {
      params.add_item(IFACE_("Grid"), [data_type](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSetGridClass");
        node.custom1 = *data_type;
        params.update_and_connect_available_socket(node, "Grid");
      });
    }
  }
}

static openvdb::GridClass grid_class_to_openvdb(const GridClass grid_class)
{
  switch (grid_class) {
    case GridClass::Unknown:
      return openvdb::GridClass::GRID_UNKNOWN;
    case GridClass::Density:
      return openvdb::GridClass::GRID_FOG_VOLUME;
    case GridClass::LevelSet:
      return openvdb::GridClass::GRID_LEVEL_SET;
    case GridClass::Staggered:
      return openvdb::GridClass::GRID_STAGGERED;
  }
  BLI_assert_unreachable();
  return openvdb::GridClass::GRID_UNKNOWN;
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid");
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }

  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.node().custom1);
  const auto grid_class = params.extract_input<GridClass>("Grid Class");
  const openvdb::GridClass vdb_grid_class = grid_class_to_openvdb(grid_class);
  const VolumeGridType grid_type = *bke::socket_type_to_grid_type(data_type);
  if (BKE_volume_is_grid_class_compatible(grid_type, vdb_grid_class)) {
    bke::VolumeGridData &grid_data = grid.get_for_write();
    bke::VolumeTreeAccessToken tree_token;
    grid_data.grid_for_write(tree_token).setGridClass(vdb_grid_class);
  }
  else {
    const char *grid_class_name, *data_type_name;
    RNA_enum_name(grid_class_items, int(grid_class), &grid_class_name);
    RNA_enum_name(rna_enum_node_socket_data_type_items, data_type, &data_type_name);
    params.error_message_add(NodeWarningType::Error,
                             "Grid of type " + std::string(data_type_name) +
                                 " cannot have class " + std::string(grid_class_name));
  }

  params.set_output("Grid", std::move(grid));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "Node socket data type",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_node_socket_data_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   return ELEM(item.value, SOCK_FLOAT, SOCK_VECTOR);
                                 });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetGridClass");
  ntype.ui_name = "Set Grid Class";
  ntype.ui_description = "Set a grid class to enable specialized handling.";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_grid_class
