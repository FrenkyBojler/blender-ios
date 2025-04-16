/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_math_matrix.hh"

#include "BKE_attribute_math.hh"
#include "BKE_pointcloud.hh"
#include "BKE_volume_fields.hh"
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

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  eCustomDataType data_type = eCustomDataType(node->custom1);

  b.add_input(data_type, "Grid");
  b.add_input<decl::Bool>("Selection").default_value(true).supports_field().hide_value();

  b.add_output<decl::Geometry>("Points").description("Point geometry representing grid voxels");
  b.add_output<decl::Vector>("Voxel Coordinate")
      .description("Index-space coordinate of the voxel")
      .field_on_all();
  b.add_output(data_type, "Value").description("Value stored in grid voxels").field_on_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
}

#ifdef WITH_OPENVDB
template<typename T> struct GridToPointsConverter {
  using type_traits = bke::VolumeGridTraits<T>;
  using TreeType = typename type_traits::TreeType;
  using GridType = openvdb::Grid<TreeType>;
  using LeafNodeType = typename TreeType::LeafNodeType;

  struct PointData {
    using type_traits = bke::VolumeGridTraits<T>;
    using LeafNodeType = typename type_traits::TreeType::LeafNodeType;

    Vector<float3> coords;
    Vector<T> values;

    /* Buffer for evaluating the selection field. */
    Array<bool> selection_buffer;

    PointData() : selection_buffer(LeafNodeType::NUM_VOXELS) {}
  };
  using ThreadPointData = threading::EnumerableThreadSpecific<PointData>;

  template<typename NodeType>
  static void add_leaf_points(const float4x4 &transform,
                              const openvdb::Coord &leaf_origin,
                              const Field<bool> &selection_field,
                              const bool store_value,
                              const NodeType &node,
                              PointData &point_data)
  {
    constexpr int num_voxels = LeafNodeType::NUM_VOXELS;

    const bke::GridLeafNodeFieldContext field_context(
        transform, int3(leaf_origin.x(), leaf_origin.y(), leaf_origin.z()));
    FieldEvaluator evaluator(field_context, field_context.size());
    evaluator.add_with_destination(selection_field, point_data.selection_buffer.as_mutable_span());
    evaluator.evaluate();
    const VArraySpan<bool> selection = evaluator.get_evaluated<bool>(0);

    /* Always record coordinates even if output is unused, we use these for counting and
     * positions as well. */
    point_data.coords.reserve(point_data.coords.size() + num_voxels);
    if (store_value) {
      point_data.values.reserve(point_data.values.size() + num_voxels);
    }

    for (const int index : IndexRange(num_voxels)) {
      if (!selection[index]) {
        continue;
      }

      const openvdb::Coord coord = leaf_origin + LeafNodeType::offsetToLocalCoord(index);
      if (!node.isValueOn(coord)) {
        continue;
      }

      const float3 fcoord = {float(coord.x()), float(coord.y()), float(coord.z())};
      point_data.coords.append(fcoord);
      if (store_value) {
        point_data.values.append(type_traits::to_blender(node.getValue(coord)));
      }
    }
  };

  static void voxels_to_points(const TreeType &vdb_tree,
                               const float4x4 &transform,
                               const Field<bool> &selection_field,
                               ThreadPointData &thread_point_data,
                               const bool store_value)
  {
    openvdb::tree::NodeManager<const TreeType> node_manager(vdb_tree);

    node_manager.foreachTopDown([&](auto &node) {
      PointData &point_data = thread_point_data.local();

      if constexpr (node.LEVEL == 0) {
        add_leaf_points(transform, node.origin(), selection_field, store_value, node, point_data);
      }
      else {
        for (auto iter = node.cbeginValueOn(); iter; ++iter) {
          const openvdb::Coord &value_origin = iter.getCoord();

          /* Number of leaves in this node. */
          using NodeType = std::decay_t<decltype(node)>;
          using ChildNodeType = typename NodeType::ChildNodeType;
          constexpr int leaves_log2dim = ChildNodeType::TOTAL - LeafNodeType::LOG2DIM;
          constexpr int leaves_dim = 1 << leaves_log2dim;
          for (const int leaf_i : IndexRange(leaves_dim)) {
            for (const int leaf_j : IndexRange(leaves_dim)) {
              for (const int leaf_k : IndexRange(leaves_dim)) {
                const openvdb::Coord leaf_offset = {leaf_i * int(LeafNodeType::DIM),
                                                    leaf_j * int(LeafNodeType::DIM),
                                                    leaf_k * int(LeafNodeType::DIM)};
                const openvdb::Coord leaf_origin = value_origin + leaf_offset;

                add_leaf_points(
                    transform, leaf_origin, selection_field, store_value, node, point_data);
              }
            }
          }
        }
      }
    });
  }

  static void grid_to_points(GeoNodeExecParams params)
  {
    const auto grid = params.extract_input<bke::VolumeGrid<T>>("Grid");
    if (!grid) {
      params.set_default_remaining_outputs();
      return;
    }
    bke::VolumeTreeAccessToken tree_token;
    const float4x4 transform = BKE_volume_transform_to_blender(grid.grid(tree_token).transform());
    typename TreeType::ConstPtr vdb_tree = grid.grid(tree_token).treePtr();

    const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

    std::optional<std::string> coord_id;
    std::optional<std::string> value_id;

    ThreadPointData thread_point_data;
    coord_id = params.get_output_anonymous_attribute_id_if_needed("Voxel Coordinate");
    value_id = params.get_output_anonymous_attribute_id_if_needed("Value");

    voxels_to_points(
        *vdb_tree, transform, selection_field, thread_point_data, value_id.has_value());

    int points_num = 0;
    for (const PointData &point_data : thread_point_data) {
      points_num += point_data.coords.size();
    }
    PointCloud *points = BKE_pointcloud_new_nomain(points_num);
    bke::MutableAttributeAccessor attributes = points->attributes_for_write();

    IndexRange points_range = {};
    MutableSpan<float3> positions = points->positions_for_write();
    SpanAttributeWriter<float3> coord_writer;
    SpanAttributeWriter<T> value_writer;
    if (coord_id) {
      coord_writer = attributes.lookup_or_add_for_write_only_span<float3>(*coord_id,
                                                                          AttrDomain::Point);
    }
    if (value_id) {
      value_writer = attributes.lookup_or_add_for_write_only_span<T>(*value_id, AttrDomain::Point);
    }

    for (const PointData &point_data : thread_point_data) {
      points_range = points_range.after(point_data.coords.size());

      for (const int i : points_range.index_range()) {
        /* Place points at voxel centers. */
        positions[points_range[i]] = math::transform_point(transform,
                                                           point_data.coords[i] + float3(0.5f));
      }
      if (coord_writer) {
        coord_writer.span.slice(points_range).copy_from(point_data.coords);
      }
      if (value_writer) {
        value_writer.span.slice(points_range).copy_from(point_data.values);
      }
    }
    coord_writer.finish();
    value_writer.finish();

    params.set_output("Points", bke::GeometrySet::from_pointcloud(points));
  }
};
#endif

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const eCustomDataType data_type = eCustomDataType(params.node().custom1);

  bke::attribute_math::convert_to_static_type(
      *bke::custom_data_type_to_cpp_type(data_type), [&](auto type_tag) {
        using ValueT = decltype(type_tag);
        using type_traits = typename bke::VolumeGridTraits<ValueT>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          GridToPointsConverter<ValueT>::grid_to_points(params);
        }
        else {
          params.set_default_remaining_outputs();
        }
      });
#else
  params.set_default_remaining_outputs();
  params.error_message_add(NodeWarningType::Error,
                           TIP_("Disabled, Blender was compiled without OpenVDB"));
#endif
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Type of grid data",
                    rna_enum_attribute_type_items,
                    NOD_inline_enum_accessors(custom1),
                    CD_PROP_FLOAT,
                    grid_attribute_type_items_filter_fn);
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGridToPoints");
  ntype.ui_name = "Grid to Points";
  ntype.ui_description = "Create a point cloud from grid voxels";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = search_link_ops_for_volume_grid_node;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_to_points_cc
