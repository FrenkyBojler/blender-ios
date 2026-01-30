/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.hh"

#include "BKE_attribute_math.hh"
#include "BKE_pointcloud.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/tree/NodeManager.h>
#endif

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
  b.add_input<decl::Bool>("Selection").default_value(true).supports_field().hide_value();
  b.add_input<decl::Menu>("Position")
      .static_items(position_mode_items)
      .default_value(PositionMode::Center)
      .expanded()
      .optional_label();

  b.add_output<decl::Geometry>("Points").description("Point geometry representing grid voxels");
  b.add_output<decl::Bool>("Is Tile").field_on_all().description(
      "Whether point represents a tile (true) or voxel (false)");
  b.add_output(data_type, "Value").field_on_all().description("Grid values at point positions");
  b.add_output(data_type, "Background").description("Background value of the grid");
  b.add_output<decl::Int>("X").field_on_all().description("X coordinate in index space");
  b.add_output<decl::Int>("Y").field_on_all().description("Y coordinate in index space");
  b.add_output<decl::Int>("Z").field_on_all().description("Z coordinate in index space");
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

  bke::attribute_math::to_static_type(
      *bke::socket_type_to_geo_nodes_base_cpp_type(data_type), [&]<typename ValueT>() {
        using type_traits = typename bke::VolumeGridTraits<ValueT>;
        using TreeType = typename type_traits::TreeType;
        using GridType = openvdb::Grid<TreeType>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          const std::shared_ptr<const GridType> vdb_grid = openvdb::GridBase::grid<GridType>(
              vdb_grid_base);
          if (!vdb_grid) {
            params.set_default_remaining_outputs();
            return;
          }

          const openvdb::math::Transform &grid_transform = vdb_grid->transform();

          std::optional<std::string> coord_x_id =
              params.get_output_anonymous_attribute_id_if_needed("X");
          std::optional<std::string> coord_y_id =
              params.get_output_anonymous_attribute_id_if_needed("Y");
          std::optional<std::string> coord_z_id =
              params.get_output_anonymous_attribute_id_if_needed("Z");
          std::optional<std::string> is_tile_id =
              params.get_output_anonymous_attribute_id_if_needed("Is Tile");
          std::optional<std::string> value_id = params.get_output_anonymous_attribute_id_if_needed(
              "Value");

          Vector<openvdb::Coord> active_coords;
          Vector<typename type_traits::BlenderType> active_values;
          Vector<bool> is_tile_flags;
          Vector<int> tile_sizes;

          for (auto iter = vdb_grid->tree().cbeginValueAll(); iter; ++iter) {
            if (iter.isValueOn()) {
              active_coords.append(iter.getCoord());
              active_values.append(type_traits::to_blender(iter.getValue()));

              const bool is_tile = iter.getLevel() > 0;
              is_tile_flags.append(is_tile);

              int tile_size = 1;
              if (is_tile) {
                tile_size = 1 << (3 * iter.getLevel());
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

          SpanAttributeWriter<bool> is_tile_writer;
          SpanAttributeWriter<int> coord_x_writer;
          SpanAttributeWriter<int> coord_y_writer;
          SpanAttributeWriter<int> coord_z_writer;
          SpanAttributeWriter<typename type_traits::BlenderType> value_writer;

          if (coord_x_id) {
            coord_x_writer = dst_attributes.lookup_or_add_for_write_only_span<int>(
                *coord_x_id, AttrDomain::Point);
          }
          if (coord_y_id) {
            coord_y_writer = dst_attributes.lookup_or_add_for_write_only_span<int>(
                *coord_y_id, AttrDomain::Point);
          }
          if (coord_z_id) {
            coord_z_writer = dst_attributes.lookup_or_add_for_write_only_span<int>(
                *coord_z_id, AttrDomain::Point);
          }
          if (is_tile_id) {
            is_tile_writer = dst_attributes.lookup_or_add_for_write_only_span<bool>(
                *is_tile_id, AttrDomain::Point);
          }
          if (value_id) {
            value_writer = dst_attributes.lookup_or_add_for_write_only_span<
                typename type_traits::BlenderType>(*value_id, AttrDomain::Point);
          }

          threading::parallel_for(active_coords.index_range(), 1024, [&](const IndexRange range) {
            for (const int64_t i : range) {
              const openvdb::Coord coord = active_coords[i];
              const int tile_size = tile_sizes[i];

              openvdb::Vec3d index_pos;
              if (position_mode == PositionMode::Center) {
                const double offset = tile_size * 0.5;
                index_pos = openvdb::Vec3d(
                    coord.x() + offset, coord.y() + offset, coord.z() + offset);
              }
              else {
                index_pos = openvdb::Vec3d(coord.x(), coord.y(), coord.z());
              }

              const openvdb::Vec3d world_pos = grid_transform.indexToWorld(index_pos);
              positions[i] = float3(
                  float(world_pos.x()), float(world_pos.y()), float(world_pos.z()));

              if (coord_x_writer) {
                coord_x_writer.span[i] = coord.x();
              }
              if (coord_y_writer) {
                coord_y_writer.span[i] = coord.y();
              }
              if (coord_z_writer) {
                coord_z_writer.span[i] = coord.z();
              }
              if (is_tile_writer) {
                is_tile_writer.span[i] = is_tile_flags[i];
              }
              if (value_writer) {
                value_writer.span[i] = active_values[i];
              }
            }
          });

          coord_x_writer.finish();
          coord_y_writer.finish();
          coord_z_writer.finish();
          is_tile_writer.finish();
          value_writer.finish();

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

            MutableAttributeAccessor filtered_attributes =
                filtered_pointcloud->attributes_for_write();
            if (coord_x_id) {
              SpanAttributeWriter<int> filtered_coord_x =
                  filtered_attributes.lookup_or_add_for_write_only_span<int>(*coord_x_id,
                                                                             AttrDomain::Point);
              array_utils::gather(coord_x_writer.span, selection_mask, filtered_coord_x.span);
              filtered_coord_x.finish();
            }
            if (coord_y_id) {
              SpanAttributeWriter<int> filtered_coord_y =
                  filtered_attributes.lookup_or_add_for_write_only_span<int>(*coord_y_id,
                                                                             AttrDomain::Point);
              array_utils::gather(coord_y_writer.span, selection_mask, filtered_coord_y.span);
              filtered_coord_y.finish();
            }
            if (coord_z_id) {
              SpanAttributeWriter<int> filtered_coord_z =
                  filtered_attributes.lookup_or_add_for_write_only_span<int>(*coord_z_id,
                                                                             AttrDomain::Point);
              array_utils::gather(coord_z_writer.span, selection_mask, filtered_coord_z.span);
              filtered_coord_z.finish();
            }
            if (is_tile_id) {
              SpanAttributeWriter<bool> filtered_is_tile =
                  filtered_attributes.lookup_or_add_for_write_only_span<bool>(*is_tile_id,
                                                                              AttrDomain::Point);
              array_utils::gather(is_tile_writer.span, selection_mask, filtered_is_tile.span);
              filtered_is_tile.finish();
            }
            if (value_id) {
              SpanAttributeWriter<typename type_traits::BlenderType> filtered_value =
                  filtered_attributes
                      .lookup_or_add_for_write_only_span<typename type_traits::BlenderType>(
                          *value_id, AttrDomain::Point);
              array_utils::gather(value_writer.span, selection_mask, filtered_value.span);
              filtered_value.finish();
            }

            geometry_set = GeometrySet::from_pointcloud(filtered_pointcloud);
          }

          params.set_output("Points", std::move(geometry_set));
          params.set_output("Background", type_traits::to_blender(vdb_grid->background()));
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
