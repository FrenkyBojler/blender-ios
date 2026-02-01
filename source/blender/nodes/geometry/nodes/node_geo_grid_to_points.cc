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

enum class OriginMode : int8_t {
  Center = 0,
  Corner = 1,
};

static const EnumPropertyItem origin_mode_items[] = {
    {int(OriginMode::Center),
     "CENTER",
     0,
     N_("Center"),
     N_("Place points at the center of voxels/tiles")},
    {int(OriginMode::Corner),
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
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Geometry>("Points").description(
      "A point for each active voxel or tile in the grid");
  b.add_output(data_type, "Value").field_on_all().description("The grid's value at each voxel");

  auto &panel = b.add_panel("Voxel Index").default_closed(true);
  panel.add_output<decl::Int>("X").field_on_all().description(
      "X coordinate of the voxel in index space, or the minimum X coordinate of a tile");
  panel.add_output<decl::Int>("Y").field_on_all().description(
      "Y coordinate of the voxel in index space, or the minimum Y coordinate of a tile");
  panel.add_output<decl::Int>("Z").field_on_all().description(
      "Z coordinate of the voxel in index space, or the minimum Z coordinate of a tile");
  panel.add_output<decl::Bool>("Is Tile").field_on_all().description(
      "If a created point represents a tile (multiple voxels) rather than a single voxel");
  panel.add_output<decl::Int>("Extent").field_on_all().description(
      "The size of the tile or voxel. For individual voxels this is 1, for tiles this represents "
      "the cubic size of the tile");

  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);
  b.add_input<decl::Menu>("Origin")
      .static_items(origin_mode_items)
      .default_value(OriginMode::Center)
      .expanded()
      .optional_label();
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
  const eNodeSocketDatatype other_type = eNodeSocketDatatype(other_socket.type);

  if (params.in_out() == SOCK_IN) {
    if (ELEM(structure_type, StructureType::Grid, StructureType::Dynamic)) {
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
  const auto origin_mode = params.extract_input<OriginMode>("Origin");
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

  const Vector<int> tile_sizes = {1, 1 << 3, 1 << (3 + 4), 1 << (3 + 4 + 5)};
  const Vector<float> tile_offsets = {0.5f, 1 << (3 - 1), 1 << (3 + 4 - 1), 1 << (3 + 4 + 5 - 1)};

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
          std::optional<std::string> extent_id =
              params.get_output_anonymous_attribute_id_if_needed("Extent");
          std::optional<std::string> value_id = params.get_output_anonymous_attribute_id_if_needed(
              "Value");

          Vector<openvdb::Coord> active_coords;
          Vector<int> levels;

          for (auto iter = vdb_grid->tree().cbeginValueOn(); iter; ++iter) {
            active_coords.append(iter.getCoord());
            levels.append(iter.getLevel());
          }

          if (active_coords.is_empty()) {
            params.set_default_remaining_outputs();
            return;
          }

          auto accessor = vdb_grid->getConstAccessor();

          PointCloud *pointcloud = BKE_pointcloud_new_nomain(active_coords.size());
          MutableSpan<float3> positions = pointcloud->positions_for_write();
          MutableAttributeAccessor dst_attributes = pointcloud->attributes_for_write();

          SpanAttributeWriter<bool> is_tile_writer;
          SpanAttributeWriter<int> coord_x_writer;
          SpanAttributeWriter<int> coord_y_writer;
          SpanAttributeWriter<int> coord_z_writer;
          SpanAttributeWriter<int> extent_writer;
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
          if (extent_id) {
            extent_writer = dst_attributes.lookup_or_add_for_write_only_span<int>(
                *extent_id, AttrDomain::Point);
          }
          if (value_id) {
            value_writer = dst_attributes.lookup_or_add_for_write_only_span<
                typename type_traits::BlenderType>(*value_id, AttrDomain::Point);
          }

          threading::parallel_for(active_coords.index_range(), 1024, [&](const IndexRange range) {
            for (const int64_t i : range) {
              const openvdb::Coord coord = active_coords[i];
              const int level = levels[i];
              const bool is_tile = level > 0;

              openvdb::Vec3d index_pos;
              if (origin_mode == OriginMode::Center) {
                const double offset = tile_offsets[level];
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
                is_tile_writer.span[i] = is_tile;
              }
              if (extent_writer) {
                extent_writer.span[i] = tile_sizes[level];
              }
              if (value_writer) {
                value_writer.span[i] = type_traits::to_blender(accessor.getValue(coord));
              }
            }
          });

          coord_x_writer.finish();
          coord_y_writer.finish();
          coord_z_writer.finish();
          is_tile_writer.finish();
          extent_writer.finish();
          value_writer.finish();

          params.set_output("Points", GeometrySet::from_pointcloud(pointcloud));
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
