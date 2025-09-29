/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_task.hh"

#include "BKE_attribute_math.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_fields.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_geo_field_to_grid.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "FN_multi_function.hh"
#include "FN_multi_function_builder.hh"

#include "../intern/volume_grid_function_eval.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "BLO_read_write.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/Dense.h>
#endif

namespace blender::nodes::node_geo_field_to_grid_cc {

NODE_STORAGE_FUNCS(NodeFieldToGrid)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const NodeFieldToGrid &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  const bool supports_fields = socket_type_supports_fields(data_type);

  b.add_default_layout();
  b.add_input(data_type, "Grid")
      .hide_value()
      .structure_type(StructureType::Grid)
      .description("Input grid to use for topology and transform information");

  const Span<NodeEnumItem> items = storage.enum_definition.items();
  for (const int i : items.index_range()) {
    const NodeEnumItem &item = items[i];
    const std::string input_identifier = FieldToGridItemsAccessor::input_socket_identifier_for_item(item);
    const std::string output_identifier = FieldToGridItemsAccessor::output_socket_identifier_for_item(item);

    auto &input = b.add_input(data_type, item.name, input_identifier);
    if (supports_fields) {
      input.supports_field();
    }
    input.description("Field value to evaluate at each grid point");

    auto &output = b.add_output(data_type, item.name, output_identifier);
    output.structure_type(StructureType::Grid)
        .align_with_previous()
        .description("Output grid with evaluated field values");
  }

  b.add_input<decl::Extend>("", "__extend__")
      .structure_type(StructureType::Dynamic)
      .custom_draw([](CustomSocketDrawParams &params) {
        uiLayout &layout = params.layout;
        layout.emboss_set(ui::EmbossType::None);
        PointerRNA op_ptr = layout.op("node.enum_definition_item_add", "", ICON_ADD);
        RNA_int_set(&op_ptr, "node_identifier", params.node.identifier);
      });
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);
  if (uiLayout *panel = layout->panel(C, "field_to_grid_items", false, IFACE_("Fields"))) {
    socket_items::ui::draw_items_list_with_operators<FieldToGridItemsAccessor>(
        C, panel, tree, node);
  }
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
  const std::optional<eNodeSocketDatatype> node_type = node_type_for_socket_type(
      params.other_socket());
  if (!node_type) {
    return;
  }
  if (params.in_out() == SOCK_IN) {
    params.add_item(IFACE_("Grid"), [node_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeFieldToGrid");
      node_storage(node).data_type = *node_type;
      params.update_and_connect_available_socket(node, "Grid");
    });
    params.add_item(IFACE_("Field"), [node_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeFieldToGrid");
      node_storage(node).data_type = *node_type;
      params.update_and_connect_available_socket(node, "Value");
    });
  }
  else {
    params.add_item(IFACE_("Grid"), [node_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeFieldToGrid");
      node_storage(node).data_type = *node_type;
      /* Connect to the first dynamic field output */
      params.update_and_connect_available_socket(node, "Value");
    });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const NodeFieldToGrid &storage = node_storage(params.node());
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  const Span<NodeEnumItem> items = storage.enum_definition.items();

  /* Handle different data types using template dispatch. */
  bke::attribute_math::convert_to_static_type(
      *bke::socket_type_to_geo_nodes_base_cpp_type(data_type), [&](auto type_tag) {
        using ValueT = decltype(type_tag);
        using type_traits = typename bke::VolumeGridTraits<ValueT>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          /* Get input grid for topology and transform. */
          const auto input_grid = params.extract_input<bke::VolumeGrid<ValueT>>("Grid");
          if (!input_grid) {
            params.set_default_remaining_outputs();
            return;
          }

          bke::VolumeTreeAccessToken token;
          const auto &source_grid = input_grid.grid(token);

          /* For each field item, evaluate the field using the grid's topology. */
          for (const int i : items.index_range()) {
            const NodeEnumItem &item = items[i];
            const std::string field_identifier = FieldToGridItemsAccessor::input_socket_identifier_for_item(item);
            const std::string grid_identifier = FieldToGridItemsAccessor::output_socket_identifier_for_item(item);

            /* Get the field input for this item. */
            Field<typename type_traits::BlenderType> input_field =
                params.extract_input<Field<typename type_traits::BlenderType>>(field_identifier);

            /* Create output grid with same transform and topology as input. */
            using OutputTreeType = typename type_traits::TreeType;
            using OutputGridType = openvdb::Grid<OutputTreeType>;
            auto output_grid = OutputGridType::create(
                type_traits::to_openvdb(typename type_traits::BlenderType{}));
            output_grid->setTransform(source_grid.transform().copy());
            output_grid->tree().topologyUnion(source_grid.tree());

            /* Evaluate field on active voxels using VoxelFieldContext. */
            Vector<openvdb::Coord> voxel_coords;
            for (auto iter = source_grid.tree().cbeginValueOn(); iter; ++iter) {
              voxel_coords.append(iter.getCoord());
            }

            if (!voxel_coords.is_empty()) {
              bke::VoxelFieldContext field_context{source_grid.transform(), voxel_coords};
              FieldEvaluator evaluator{field_context, voxel_coords.size()};
              Array<typename type_traits::BlenderType> values(voxel_coords.size());
              evaluator.add_with_destination(input_field, values.as_mutable_span());
              evaluator.evaluate();

              /* Write values to output grid. */
              auto accessor = output_grid->getAccessor();
              for (int64_t j = 0; j < voxel_coords.size(); j++) {
                accessor.setValue(voxel_coords[j], type_traits::to_openvdb(values[j]));
              }
            }

            bke::VolumeGrid<ValueT> result_grid(std::move(output_grid));
            params.set_output(grid_identifier, std::move(result_grid));
          }
        }
      });
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeFieldToGrid *data = MEM_callocN<NodeFieldToGrid>(__func__);
  data->data_type = SOCK_FLOAT;
  data->enum_definition.next_identifier = 0;

  node->storage = data;

  socket_items::add_item_with_name<FieldToGridItemsAccessor>(*node, "Value");
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Node socket data type",
                    rna_enum_node_socket_data_type_items,
                    NOD_storage_enum_accessors(data_type),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<FieldToGridItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeFieldToGrid &src_storage = node_storage(*src_node);
  NodeFieldToGrid *dst_storage = MEM_dupallocN<NodeFieldToGrid>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<FieldToGridItemsAccessor>(*src_node, *dst_node);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<FieldToGridItemsAccessor>();
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<FieldToGridItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<FieldToGridItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<FieldToGridItemsAccessor>(&reader, node);
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  const NodeFieldToGrid &storage = node_storage(node);
  if (storage.enum_definition.items_num == 0) {
    return nullptr;
  }

  /* Find the corresponding field input for each grid output */
  const Span<NodeEnumItem> items = storage.enum_definition.items();
  for (const int i : items.index_range()) {
    const NodeEnumItem &item = items[i];
    const std::string grid_identifier = FieldToGridItemsAccessor::output_socket_identifier_for_item(item);

    /* Check if this output socket matches this item's grid output */
    if (output_socket.identifier == grid_identifier) {
      const std::string field_identifier = FieldToGridItemsAccessor::input_socket_identifier_for_item(item);
      /* Find and return the corresponding field input socket */
      return node.input_by_identifier(field_identifier);
    }
  }

  return nullptr;
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFieldToGrid");
  ntype.ui_name = "Field to Grid";
  ntype.ui_description = "Evaluate fields at grid points and output new grids with the results";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeFieldToGrid", node_free_storage, node_copy_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.insert_link = node_insert_link;
  ntype.ignore_inferred_input_socket_visibility = true;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.internally_linked_input = node_internally_linked_input;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_field_to_grid_cc

namespace blender::nodes {

StructRNA *FieldToGridItemsAccessor::item_srna = &RNA_FieldToGridItem;

void FieldToGridItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
  BLO_write_string(writer, item.description);
}

void FieldToGridItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
  BLO_read_string(reader, &item.description);
}

}  // namespace blender::nodes
