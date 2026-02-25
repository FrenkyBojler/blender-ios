/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume.hh"
#include "BKE_volume_enums.hh"
#include "BKE_volume_grid.hh"

#include "BLI_generic_array.hh"
#include "BLI_math_matrix.hh"

#include "BLO_read_write.hh"

#include "GEO_points_to_volume.hh"

#include "NOD_geo_points_to_grid.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_points_to_density_grid_cc {

using bke::RasterizePointsWeighting;

NODE_STORAGE_FUNCS(NodeGeometryPointsToDensityGrid)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Points");
  b.add_input<decl::Vector>("Position").implicit_field_on_all(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Float>("Voxel Size").default_value(0.3f).min(0.01f).subtype(PROP_DISTANCE);

  b.add_input<decl::Float>("Mass").default_value(1.0f).min(0.0f).supports_field();
  b.add_output<decl::Float>("Mass").structure_type(StructureType::Grid).align_with_previous();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree) {
    const NodeGeometryPointsToDensityGrid &storage = node_storage(*node);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeGridItem &item = storage.items[i];
      const StringRef name = item.name ? item.name : "";
      const std::string identifier = GridItemsAccessor::socket_identifier_for_item(item);
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);

      auto &input_decl = b.add_input(socket_type, name, identifier);
      input_decl.socket_name_ptr(&tree->id, GridItemsAccessor::item_srna, &item, "name");
      input_decl.supports_field();

      eNodeSocketDatatype output_socket_type = socket_type;
      /* Special case: affine transform attribute is converted to vector grid. */
      if (socket_type == SOCK_MATRIX && (item.flag & GEO_NODE_GRID_ITEM_AFFINE_VECTOR)) {
        output_socket_type = SOCK_VECTOR;
      }
      b.add_output(output_socket_type, name, identifier)
          .structure_type(StructureType::Grid)
          .align_with_previous();
    }
  }
  b.add_input<decl::Extend>("", "__extend__");
  b.add_output<decl::Extend>("", "__extend__").align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryPointsToDensityGrid *data = MEM_callocN<NodeGeometryPointsToDensityGrid>(__func__);
  data->next_identifier = 0;

  data->items = nullptr;
  data->items_num = 0;

  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<GridItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryPointsToDensityGrid &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeGeometryPointsToDensityGrid>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<GridItemsAccessor>(*src_node, *dst_node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<GridItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<GridItemsAccessor>();
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);
  if (uiLayout *panel = layout->panel(C, "grid_items", false, IFACE_("Items"))) {
    socket_items::ui::draw_items_list_with_operators<GridItemsAccessor>(C, panel, ntree, node);
    socket_items::ui::draw_active_item_props<GridItemsAccessor>(
        ntree, node, [&](PointerRNA *item_ptr) {
          const NodeGridItem &item = *item_ptr->data_as<NodeGridItem>();
          const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);

          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          panel->prop(item_ptr, "weighting", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          if (socket_type == SOCK_VECTOR) {
            panel->prop(item_ptr, "use_staggered_vector", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          }
          if (socket_type == SOCK_MATRIX) {
            panel->prop(item_ptr, "use_affine_vector", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          }
        });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  constexpr StringRef mass_attribute = "mass";

  const NodeGeometryPointsToDensityGrid &storage = node_storage(params.node());
  const geometry::KernelType kernel_type = geometry::KernelType::Cubic;

  const float voxel_size = params.extract_input<float>("Voxel Size");
  const double determinant = std::pow(double(voxel_size), 3.0);
  if (!BKE_volume_grid_determinant_valid(determinant)) {
    params.set_default_remaining_outputs();
    return;
  }
  /* Same transform is used for the intermediate point data grid and all output grids. */
  const float4x4 grid_transform = math::from_scale<float4x4>(float3(voxel_size));

  const GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  const Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  const Field<float> mass_field = params.extract_input<Field<float>>("Mass");

  const Array<GeometryComponent::Type> component_types = {GeometryComponent::Type::Mesh,
                                                          GeometryComponent::Type::PointCloud,
                                                          GeometryComponent::Type::Curve};
  Vector<int> points_offsets;
  for (const GeometryComponent::Type type : component_types) {
    const GeometryComponent *component = geometry_set.get_component(type);
    points_offsets.append(component ? component->attribute_domain_size(AttrDomain::Point) : 0);
  }
  points_offsets.append(0);
  const OffsetIndices points_by_component = offset_indices::accumulate_counts_to_offsets(
      points_offsets);
  const int points_num = points_by_component.total_size();
  if (points_num == 0) {
    params.set_default_remaining_outputs();
    return;
  }

  Array<float3> positions(points_num);
  Array<float> masses(points_num);
  Array<GArray<>> value_buffers(storage.items_num);
  Vector<geometry::PointDataGridAttributeInfo> point_data_grid_attributes;
  Vector<geometry::PointRasterizeAttributeInfo> point_rasterize_attributes;
  point_data_grid_attributes.append({mass_attribute, masses.as_span()});
  for (const int i : IndexRange(storage.items_num)) {
    const NodeGridItem &item = storage.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const RasterizePointsWeighting rasterize_weighting = RasterizePointsWeighting(item.weighting);
    const CPPType &cpptype = *bke::socket_type_to_geo_nodes_base_cpp_type(socket_type);
    const bool use_staggered_vector = (item.flag & GEO_NODE_GRID_ITEM_VECTOR_STAGGERED);
    const bool use_affine_vector = (item.flag & GEO_NODE_GRID_ITEM_AFFINE_VECTOR);

    value_buffers[i] = GArray<>(cpptype, points_num);
    /* Note: Item name is unique and can be used as an attribute identifier. */
    point_data_grid_attributes.append({item.name, value_buffers[i].as_span()});
    point_rasterize_attributes.append(
        {item.name, cpptype, rasterize_weighting, use_staggered_vector, use_affine_vector});
  }

  for (const int component_i : component_types.index_range()) {
    const GeometryComponent *component = geometry_set.get_component(component_types[component_i]);
    const IndexRange points = points_by_component[component_i];
    if (points.is_empty()) {
      continue;
    }
    BLI_assert(component != nullptr);

    const bke::GeometryFieldContext field_context{*component, AttrDomain::Point};
    fn::FieldEvaluator evaluator{field_context, points.size()};
    evaluator.add_with_destination(position_field, positions.as_mutable_span().slice(points));
    evaluator.add_with_destination(mass_field, masses.as_mutable_span().slice(points));
    for (const int i : IndexRange(storage.items_num)) {
      const NodeGridItem &item = storage.items[i];
      const std::string identifier = GridItemsAccessor::socket_identifier_for_item(item);

      evaluator.add_with_destination(params.extract_input<GField>(identifier),
                                     value_buffers[i].as_mutable_span().slice(points));
    }
    evaluator.evaluate();
  }

  geometry::MappedPointDataGrid point_data_grid = geometry::points_to_point_data_grid(
      positions, point_data_grid_attributes, grid_transform);

  std::optional<bke::GVolumeGrid> output_mass_grid = params.output_is_required("Mass") ?
                                                         std::make_optional<bke::GVolumeGrid>() :
                                                         std::nullopt;
  // TODO only generate output grids that are actually needed.
  Array<bke::GVolumeGrid> output_attribute_grids(storage.items_num);
  geometry::points_rasterize(point_data_grid,
                             kernel_type,
                             mass_attribute,
                             point_rasterize_attributes,
                             grid_transform,
                             output_mass_grid,
                             output_attribute_grids);
  if (output_mass_grid) {
    params.set_output("Mass", std::move(*output_mass_grid));
  }
  for (const int i : IndexRange(storage.items_num)) {
    const NodeGridItem &item = storage.items[i];
    const std::string identifier = GridItemsAccessor::socket_identifier_for_item(item);
    BLI_assert(output_attribute_grids[i]);
    params.set_output(identifier, std::move(output_attribute_grids[i]));
  }

#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<GridItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<GridItemsAccessor>(&reader, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodePointsToDensityGrid", GEO_NODE_POINTS_TO_DENSITY_GRID);
  ntype.ui_name = "Points to Density Grid";
  ntype.ui_description = "Create volume grids from points with a weighted sum";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.enum_name_legacy = "POINTS_TO_DENSITY_GRID";
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryPointsToDensityGrid", node_free_storage, node_copy_storage);
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = search_link_ops_for_volume_grid_node;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_points_to_density_grid_cc

namespace blender::nodes {

StructRNA *GridItemsAccessor::item_srna = &RNA_NodeGridItem;
int GridItemsAccessor::node_type = GEO_NODE_POINTS_TO_DENSITY_GRID;

void GridItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void GridItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

// TODO
// std::string GridItemsAccessor::custom_initial_name(const bNode &node, StringRef src_name)
// {
//   /* The goal is to find a single-letter name that is not used already. Ideally, it starts with
//   the
//    * same letter as the given name. */

//   const auto &storage = *static_cast<NodeFunctionFormatString *>(node.storage);
//   char initial = 'a';
//   if (!src_name.is_empty()) {
//     const char first_c = src_name[0];
//     if (first_c >= 'a' && first_c <= 'z') {
//       initial = first_c;
//     }
//     else if (first_c >= 'A' && first_c <= 'Z') {
//       initial = first_c - 'A' + 'a';
//     }
//   }
//   for (const int i : IndexRange('z' - 'a' + 1)) {
//     char c = initial + i;
//     if (c > 'z') {
//       /* Start at 'a' again. */
//       c = c - 'z' + 'a' - 1;
//     }
//     const std::string potential_name = std::string(1, c);
//     const bool name_exists = std::any_of(
//         storage.items,
//         storage.items + storage.items_num,
//         [&](const NodeFunctionFormatStringItem &item) { return item.name == potential_name; });
//     if (!name_exists) {
//       return potential_name;
//     }
//   }
//   return src_name;
// }

// std::string GridItemsAccessor::validate_name(const StringRef name)
// {
//   /* The name has to start with a letter or underscore. The remaining letters may additionally
//   be
//    * digits. */
//   std::string result;
//   if (name.is_empty()) {
//     return result;
//   }
//   const char first_char = name[0];
//   if (!std::isalpha(first_char) && first_char != '_') {
//     result += '_';
//   }
//   for (const char c : name) {
//     if (std::isalnum(c) || c == '_') {
//       result += c;
//     }
//     if (ELEM(c, '-', '.', ' ', '\t')) {
//       result += '_';
//     }
//   }

//   return result;
// }

}  // namespace blender::nodes

blender::Span<NodeGridItem> NodeGeometryPointsToDensityGrid::items_span() const
{
  return blender::Span<NodeGridItem>(items, items_num);
}

blender::MutableSpan<NodeGridItem> NodeGeometryPointsToDensityGrid::items_span()
{
  return blender::MutableSpan<NodeGridItem>(items, items_num);
}
