/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/tools/Prune.h>
#endif

namespace blender::nodes::node_geo_grid_topology_boolean {

enum class Operation {
  Intersect = 0,
  Union = 1,
  Difference = 2,
};

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
  const Operation operation = Operation(node->custom2);

  auto &first_grid =
      b.add_input(data_type, "Grid 1").hide_value().structure_type(StructureType::Grid);

  static const auto make_available = [](bNode &node) {
    node.custom2 = int16_t(Operation::Difference);
  };
  switch (operation) {
    case Operation::Intersect:
    case Operation::Union:
      b.add_input(data_type, "Grid", "Grid 2")
          .hide_value()
          .multi_input()
          .make_available(make_available)
          .structure_type(StructureType::Grid);
      break;
    case Operation::Difference:
      b.add_input(data_type, "Grid 2")
          .hide_value()
          .multi_input()
          .make_available(make_available)
          .structure_type(StructureType::Grid);
      break;
  }

  b.add_output(data_type, "Grid").hide_value().structure_type(StructureType::Grid);

  if (node) {
    switch (Operation(node->custom2)) {
      case Operation::Intersect:
      case Operation::Union:
        first_grid.available(false);
        break;
      case Operation::Difference:
        first_grid.available(true);
        break;
    }
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
  node->custom2 = int16_t(Operation::Difference);
}

static void node_rna(StructRNA *srna)
{

  static const EnumPropertyItem operation_items[] = {
      {int(Operation::Intersect),
       "INTERSECT",
       0,
       "Intersect",
       "Keep the part of the grids that is common between all operands"},
      {int(Operation::Union), "UNION", 0, "Union", "Combine grids in an additive way"},
      {int(Operation::Difference),
       "DIFFERENCE",
       0,
       "Difference",
       "Combine grids in a subtractive way"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Node socket data type",
                    rna_enum_node_socket_data_type_items,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);

  RNA_def_node_enum(srna,
                    "operation",
                    "Operation",
                    "",
                    operation_items,
                    NOD_inline_enum_accessors(custom2),
                    int(Operation::Difference));
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.node().custom1);
  const Operation operation = Operation(params.node().custom2);
  auto grids = params.extract_input<GeoNodesMultiInput<bke::GVolumeGrid>>("Grid 2"_ustr);
  Vector<bke::GVolumeGrid> operands;
  switch (operation) {
    case Operation::Intersect:
    case Operation::Union:
      operands.extend(grids.values);
      break;
    case Operation::Difference:
      if (auto grid = params.extract_input<bke::GVolumeGrid>("Grid 1"_ustr)) {
        operands.append(std::move(grid));
      }
      operands.extend(grids.values);
      break;
  }

  if (operands.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  const VolumeGridType grid_type = *bke::socket_type_to_grid_type(data_type);
  BKE_volume_grid_type_to_static_type(
      grid_type, [&]<std::derived_from<openvdb::GridBase> GridType>() {
        if constexpr (std::is_same_v<GridType, openvdb::FloatGrid> ||
                      std::is_same_v<GridType, openvdb::Int32Grid> ||
                      std::is_same_v<GridType, openvdb::Vec3fGrid>)
        {
          bke::VolumeTreeAccessToken result_token;
          GridType &result_grid = static_cast<GridType &>(
              operands.first().get_for_write().grid_for_write(result_token));
          const openvdb::math::Transform &transform = result_grid.transform();

          for (bke::GVolumeGrid &volume_grid : operands.as_mutable_span().drop_front(1)) {
            bke::VolumeTreeAccessToken operand_token;
            const GridType &operand_grid = static_cast<const GridType &>(
                volume_grid.get().grid(operand_token));

            if (operand_grid.transform() != transform) {
              params.error_message_add(NodeWarningType::Warning,
                                       TIP_("Mismatched grid operand transforms"));
            }

            try {
              switch (operation) {
                case Operation::Intersect:
                  result_grid.tree().topologyIntersection(operand_grid.tree());
                  openvdb::tools::pruneInactive(result_grid.tree());
                  break;
                case Operation::Union:
                  result_grid.tree().topologyUnion(operand_grid.tree());
                  break;
                case Operation::Difference:
                  result_grid.tree().topologyDifference(operand_grid.tree());
                  openvdb::tools::pruneInactive(result_grid.tree());
                  break;
              }
            }
            catch (const openvdb::ValueError & /*ex*/) {
              /* May happen if a grid is empty. */
              params.set_default_remaining_outputs();
              return;
            }
          }
          operands.first()->tag_tree_modified();
        }
      });

  params.set_output("Grid"_ustr, std::move(operands.first()));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGridTopologyBoolean");
  ntype.ui_name = "Grid Topology Boolean";
  ntype.ui_description = "Combine the topology of multiple grids";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_topology_boolean
