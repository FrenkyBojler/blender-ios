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

  b.add_input(data_type, "Grid")
      .hide_value()
      .structure_type(StructureType::Grid)
      .description("Input grid to use for topology and transform information");

  b.add_default_layout();

  const Span<FieldToGridItem> items = storage.items_span();
  for (const int i : items.index_range()) {
    const FieldToGridItem &item = items[i];
    const std::string identifier = FieldToGridItemsAccessor::socket_identifier_for_item(item);
    
    auto &input = b.add_input(data_type, item.name, identifier + "_field");
    if (supports_fields) {
      input.supports_field();
    }
    input.description("Field value to evaluate at each grid point");

    auto &output = b.add_output(data_type, item.name, identifier + "_grid");
    output.structure_type(StructureType::Grid)
          .align_with_previous()
          .description("Output grid with evaluated field values");
  }

  b.add_input<decl::Extend>("", "__extend__").custom_draw([](CustomSocketDrawParams &params) {
    uiLayout &layout = params.layout;
    layout.emboss_set(ui::EmbossType::None);
    PointerRNA op_ptr = layout.op("node.field_to_grid_item_add", IFACE_(""), ICON_ADD);
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
  bNode &node = *static_cast<bNode *>(ptr->data);
  NodeFieldToGrid &storage = node_storage(node);
  if (uiLayout *panel = layout->panel(C, "field_to_grid_items", false, IFACE_("Fields"))) {
    panel->op("node.field_to_grid_item_add", IFACE_("Add Field"), ICON_ADD);
    uiLayout *col = &panel->column(false);
    for (const int i : IndexRange(storage.items_num)) {
      uiLayout *row = &col->row(false);
      row->label(storage.items[i].name, ICON_NONE);
      PointerRNA op_ptr = row->op("node.field_to_grid_item_remove", "", ICON_REMOVE);
      RNA_int_set(&op_ptr, "index", i);
    }
  }
}

static void NODE_OT_field_to_grid_item_add(wmOperatorType *ot)
{
  socket_items::ops::add_item<FieldToGridItemsAccessor>(ot, "Add Field", __func__, "Add field to evaluate");
}

static void NODE_OT_field_to_grid_item_remove(wmOperatorType *ot)
{
  socket_items::ops::remove_item_by_index<FieldToGridItemsAccessor>(
      ot, "Remove Field", __func__, "Remove a field from the evaluation");
}

static void node_operators()
{
  WM_operatortype_append(NODE_OT_field_to_grid_item_add);
  WM_operatortype_append(NODE_OT_field_to_grid_item_remove);
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const NodeFieldToGrid &storage = node_storage(params.node());
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  const Span<FieldToGridItem> items = storage.items_span();

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
            const FieldToGridItem &item = items[i];
            const std::string identifier = FieldToGridItemsAccessor::socket_identifier_for_item(item);
            const std::string field_identifier = identifier + "_field";
            const std::string grid_identifier = identifier + "_grid";

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
  data->next_identifier = 0;

  BLI_assert(data->items == nullptr);
  const int default_items_num = 1;
  data->items = MEM_calloc_arrayN<FieldToGridItem>(default_items_num, __func__);
  data->items[0].identifier = data->next_identifier++;
  STRNCPY(data->items[0].name, "Value");
  data->items_num = default_items_num;

  node->storage = data;
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
  auto *dst_storage = MEM_dupallocN<NodeFieldToGrid>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<FieldToGridItemsAccessor>(*src_node, *dst_node);
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

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFieldToGrid");
  ntype.ui_name = "Field to Grid";
  ntype.ui_description = "Evaluate fields at grid points and output new grids with the results";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.insert_link = node_insert_link;
  blender::bke::node_type_storage(ntype, "NodeFieldToGrid", node_free_storage, node_copy_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_field_to_grid_cc

namespace blender::nodes {

StructRNA *FieldToGridItemsAccessor::item_srna = &RNA_FieldToGridItem;

void FieldToGridItemsAccessor::blend_write_item(BlendWriter * /*writer*/, const ItemT & /*item*/)
{
}

void FieldToGridItemsAccessor::blend_read_data_item(BlendDataReader * /*reader*/, ItemT & /*item*/)
{
}

}  // namespace blender::nodes

blender::Span<FieldToGridItem> NodeFieldToGrid::items_span() const
{
  return blender::Span<FieldToGridItem>(items, items_num);
}

blender::MutableSpan<FieldToGridItem> NodeFieldToGrid::items_span()
{
  return blender::MutableSpan<FieldToGridItem>(items, items_num);
}
