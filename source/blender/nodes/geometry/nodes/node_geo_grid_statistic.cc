/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/Count.h>
#endif

#include "BKE_lib_id.hh"
#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_type_traits.hh"

#include "RNA_enum_types.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_geo_grid_statistic_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);

  b.add_output(data_type, "Min");
  b.add_output(data_type, "Max");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

#ifdef WITH_OPENVDB

static void node_geo_exec(GeoNodeExecParams params)
{
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.node().custom1);

  const bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid");

  bke::attribute_math::convert_to_static_type(
      *bke::socket_type_to_geo_nodes_base_cpp_type(data_type), [&](auto type_tag) {
        using ValueT = decltype(type_tag);
        using type_traits = typename bke::VolumeGridTraits<ValueT>;
        using TreeType = typename type_traits::TreeType;
        using GridType = openvdb::Grid<TreeType>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          bke::VolumeTreeAccessToken tree_token;
          const auto bounds = openvdb::tools::minMax<TreeType>(
              grid.typed<ValueT>().grid(tree_token).tree());

          params.set_output<ValueT>("Min",
                                    bke::VolumeGridTraits<ValueT>::to_blender(bounds.min()));
          params.set_output<ValueT>("Max",
                                    bke::VolumeGridTraits<ValueT>::to_blender(bounds.max()));
        }
        else {
          BLI_assert(false);
        }
      });
}

#else /* WITH_OPENVDB */

static void node_geo_exec(GeoNodeExecParams params)
{
  node_geo_exec_with_missing_openvdb(params);
}

#endif /* WITH_OPENVDB */

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Type of grid data",
                    rna_enum_node_socket_data_type_items,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGridStatistic");
  ntype.ui_name = "Grid Statistic";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_statistic_cc
