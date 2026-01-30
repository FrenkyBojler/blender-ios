/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"
#include "BKE_pointcloud.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.hh"
#include "BLI_task.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "FN_multi_function_builder.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grid_to_points_cc {

enum class PositionMode : int8_t {
  Center = 0,
  Corner = 1,
};

static const EnumPropertyItem position_mode_items[] = {
    {int(PositionMode::Center),
     "CENTER",
     0,
     N_("Center"),
     N_("Place points at the center of voxels/tiles")},
    {int(PositionMode::Corner),
     "CORNER",
     0,
     N_("Corner"),
     N_("Place points at the corner of voxels/tiles")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();
  b.add_input<decl::Menu>("Position")
      .static_items(position_mode_items)
      .default_value(PositionMode::Center);

  b.add_output<decl::Geometry>("Points").propagate_all();
  b.add_output<decl::Bool>("Is Tile").field_on_all().description("Whether point represents a tile (true) or voxel (false)");
  b.add_output(data_type, "Value").field_on_all().description("Grid values at point positions");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
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
  const eNodeSocketDatatype other_type = eNodeSocketDatatype(other_socket.type);

  if (params.in_out() == SOCK_IN) {
    if (is_grid || is_dynamic) {
      const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(other_socket);
      if (data_type) {
        params.add_item(IFACE_("Grid"), [data_type](LinkSearchOpParams &params) {
          bNode &node = params.add_node("GeometryNodeGridToPoints");
          node.custom1 = *data_type;
          params.update_and_connect_available_socket(node, "Grid");
        });
      }
    }
  }
  else {
    if (params.node_tree().typeinfo->validate_link(SOCK_GEOMETRY, other_type)) {
      params.add_item(IFACE_("Points"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeGridToPoints");
        params.update_and_connect_available_socket(node, "Points");
      });
    }
    const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(other_socket);
    if (data_type) {
      params.add_item(IFACE_("Value"), [data_type](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeGridToPoints");
        node.custom1 = *data_type;
        params.update_and_connect_available_socket(node, "Value");
      });
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.node().custom1);
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  const PositionMode position_mode = PositionMode(params.extract_input<int>("Position"));

  bke::attribute_math::to_static_type(
      *bke::socket_type_to_geo_nodes_base_cpp_type(data_type), [&]<typename ValueT>() {
        using type_traits = typename bke::VolumeGridTraits<ValueT>;
        using TreeType = typename type_traits::TreeType;
        using GridType = openvdb::Grid<TreeType>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          const auto grid = params.extract_input<bke::GVolumeGrid>("Grid");
          if (!grid) {
            params.set_default_remaining_outputs();
            return;
          }

          bke::VolumeTreeAccessToken tree_token;
          const std::shared_ptr<const openvdb::GridBase> vdb_grid_base = grid->grid_ptr(tree_token);
          if (!vdb_grid_base) {
            params.set_default_remaining_outputs();
            return;
          }

          const std::shared_ptr<const GridType> vdb_grid = openvdb::GridBase::grid<GridType>(
              vdb_grid_base);
          if (!vdb_grid) {
            params.set_default_remaining_outputs();
            return;
          }

          const openvdb::math::Transform &grid_transform = vdb_grid->transform();

          Vector<openvdb::Coord> active_coords;
          Vector<typename type_traits::BlenderType> active_values;
          Vector<bool> is_tile_flags;
          Vector<int> tile_sizes;  /* Store the size of each tile for proper centering */

          /* Iterate through all active values including both voxels and tiles */
          for (auto iter = vdb_grid->tree().cbeginValueAll(); iter; ++iter) {
            if (iter.isValueOn()) {
              active_coords.append(iter.getCoord());
              active_values.append(type_traits::to_blender(iter.getValue()));

              const bool is_tile = iter.getLevel() > 0;
              is_tile_flags.append(is_tile);

              /* Calculate tile size: tiles at higher levels represent larger cubic regions */
              int tile_size = 1;
              if (is_tile) {
                /* Each level up multiplies the size by the branching factor */
                /* For OpenVDB default trees, leaf nodes are 8^3, internal nodes vary */
                tile_size = 1 << (3 * iter.getLevel());  /* 2^(3*level) gives cubic tile size */
              }
              tile_sizes.append(tile_size);
            }
          }

          if (active_coords.is_empty()) {
            params.set_default_remaining_outputs();
            return;
          }

          PointCloud *pointcloud = BKE_pointcloud_new_nomain(active_coords.size());
          MutableSpan<float3> positions = pointcloud->positions_for_write();
          MutableAttributeAccessor dst_attributes = pointcloud->attributes_for_write();

          SpanAttributeWriter<typename type_traits::BlenderType> values =
              dst_attributes.lookup_or_add_for_write_only_span<typename type_traits::BlenderType>(
                  "value", AttrDomain::Point);
          SpanAttributeWriter<bool> is_tile =
              dst_attributes.lookup_or_add_for_write_only_span<bool>("is_tile", AttrDomain::Point);

          threading::parallel_for(active_coords.index_range(), 1024, [&](const IndexRange range) {
            for (const int64_t i : range) {
              const openvdb::Coord coord = active_coords[i];
              const int tile_size = tile_sizes[i];

              /* Calculate position based on mode */
              openvdb::Vec3d index_pos;
              if (position_mode == PositionMode::Center) {
                /* For tiles, center within the tile's cubic region */
                const double offset = tile_size * 0.5;
                index_pos = openvdb::Vec3d(coord.x() + offset, coord.y() + offset, coord.z() + offset);
              } else {
                /* Use corner position (coord represents the min corner of the tile/voxel) */
                index_pos = openvdb::Vec3d(coord.x(), coord.y(), coord.z());
              }

              const openvdb::Vec3d world_pos = grid_transform.indexToWorld(index_pos);
              positions[i] = float3(float(world_pos.x()), float(world_pos.y()), float(world_pos.z()));
              values.span[i] = active_values[i];
              is_tile.span[i] = is_tile_flags[i];
            }
          });

          values.finish();
          is_tile.finish();

          GeometrySet geometry_set = GeometrySet::from_pointcloud(pointcloud);

          const bke::PointCloudFieldContext field_context{*pointcloud};
          fn::FieldEvaluator evaluator{field_context, pointcloud->totpoint};
          evaluator.set_selection(selection);
          evaluator.evaluate();
          const IndexMask selection_mask = evaluator.get_evaluated_selection_as_mask();

          if (selection_mask.size() != pointcloud->totpoint) {
            PointCloud *filtered_pointcloud = BKE_pointcloud_new_nomain(selection_mask.size());

            MutableSpan<float3> filtered_positions = filtered_pointcloud->positions_for_write();
            array_utils::gather(positions, selection_mask, filtered_positions);

            MutableAttributeAccessor filtered_dst_attributes = filtered_pointcloud->attributes_for_write();
            SpanAttributeWriter<typename type_traits::BlenderType> filtered_values =
                filtered_dst_attributes.lookup_or_add_for_write_only_span<typename type_traits::BlenderType>(
                    "value", AttrDomain::Point);
            SpanAttributeWriter<bool> filtered_is_tile =
                filtered_dst_attributes.lookup_or_add_for_write_only_span<bool>("is_tile", AttrDomain::Point);

            array_utils::gather(values.span, selection_mask, filtered_values.span);
            array_utils::gather(is_tile.span, selection_mask, filtered_is_tile.span);
            filtered_values.finish();
            filtered_is_tile.finish();

            geometry_set = GeometrySet::from_pointcloud(filtered_pointcloud);
          }

          params.set_output("Points", std::move(geometry_set));
          
          Field<bool> is_tile_field = AttributeFieldInput::from<bool>("is_tile");
          params.set_output("Is Tile", std::move(is_tile_field));
          
          Field<typename type_traits::BlenderType> value_field = AttributeFieldInput::from<typename type_traits::BlenderType>("value");
          params.set_output("Value", std::move(value_field));
        }
      });
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
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGridToPoints");
  ntype.ui_name = "Grid to Points";
  ntype.ui_description = "Generate a point cloud from a volume grid's active voxels";
  ntype.enum_name_legacy = "GRID_TO_POINTS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_to_points_cc
