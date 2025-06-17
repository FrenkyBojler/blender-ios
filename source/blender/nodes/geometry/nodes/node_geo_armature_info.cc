/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "BKE_armature.hh"
#include "BKE_action.hh"
#include "node_geometry_util.hh"
#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_geo_armature_info.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_prototypes.hh"

#include "BLO_read_write.hh"

#include "BKE_node_socket_value.hh"

#include "DEG_depsgraph_query.hh"
#include "DEG_depsgraph.hh"
#include "BLI_listbase.h"
#include "DEG_depsgraph_build.hh"
#include "BKE_node.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_armature_info_cc {

NODE_STORAGE_FUNCS(NodeGeometryArmatureInfo)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const NodeGeometryArmatureInfo &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  const Span<ArmatureInfoItem> items = storage.items_span();

  b.add_input<decl::Object>("Object").hide_label();
  b.add_output<decl::Bool>("Is Armature").description("Returns true if the object is an armature, otherwise false");

  for (const int i : items.index_range()) {
    const std::string base_identifier = ArmatureInfoItemsAccessor::socket_identifier_for_item(items[i]);
    const std::string input_identifier = "in_" + base_identifier;
    const std::string output_identifier = "out_" + base_identifier;
    auto &input = b.add_input(data_type, std::to_string(i), input_identifier);
    b.add_output(SOCK_VECTOR, std::to_string(i), output_identifier);
  }
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  // Temporarily commented out to resolve compilation errors
  /*
  bNodeSocket *sock = nullptr;
  for (bNodeSocket *s = (bNodeSocket *)node->inputs.first; s; s = s->next) {
      if (STREQ(s->name, "Object")) {
          sock = s;
          break;
      }
  }
  if (sock) {
      Object *ob = nullptr;
      if (sock->link) {
          bNodeLink *link = sock->link;
          if (link->fromnode && link->fromsock) {
              bNodeSocketValueObject *value = (bNodeSocketValueObject *)link->fromsock->default_value;
              if (value) {
                  ob = value->value;
              }
          }
      } else {
          bNodeSocketValueObject *value = (bNodeSocketValueObject *)sock->default_value;
          if (value) {
              ob = value->value;
          }
      }
      if (ob && ob->type == OB_ARMATURE) {
          Depsgraph *depsgraph = ntree->id.depsgraph;
          if (depsgraph) {
              DepsNodeHandle handle;
              DEG_node_handle_init(&handle, depsgraph, node);
              DEG_add_object_relation(&handle, ob, DEG_OB_COMP_EVAL_POSE, "Armature Info Node");
          }
      }
  }
  */
  UNUSED_VARS(ntree, node);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *object = params.extract_input<Object *>("Object");
  const bool is_armature = (object && object->type == OB_ARMATURE);
  params.set_output("Is Armature", is_armature);

  const bNode &node = params.node();
  const NodeGeometryArmatureInfo &storage = node_storage(node);
  const Span<ArmatureInfoItem> items = storage.items_span();

  if (!object || items.is_empty()) {
    for (const int i : items.index_range()) {
      const std::string base_identifier = ArmatureInfoItemsAccessor::socket_identifier_for_item(items[i]);
      const std::string socket_name = "out_" + base_identifier;
      params.set_output(socket_name, float3(0.0f));
    }
    return;
  }

  if (!is_armature) {
    for (const int i : items.index_range()) {
      const std::string base_identifier = ArmatureInfoItemsAccessor::socket_identifier_for_item(items[i]);
      const std::string socket_name = "out_" + base_identifier;
      params.set_output(socket_name, float3(0.0f));
    }
    return;
  }

  const Depsgraph *depsgraph = params.depsgraph();
  if (!depsgraph) {
    params.error_message_add(NodeWarningType::Error, 
        "No depsgraph available - cannot access evaluated data");
    return;
  }
  
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  if (!object_eval || !object_eval->pose) {
    params.error_message_add(NodeWarningType::Error, 
        "Could not access evaluated pose data");
    return;
  }

  for (const int i : items.index_range()) {
    const std::string base_identifier = ArmatureInfoItemsAccessor::socket_identifier_for_item(items[i]);
    const std::string input_identifier = "in_" + base_identifier;
    const std::string socket_name = "out_" + base_identifier;

    std::string bone_name;
    try {
      bone_name = params.extract_input<std::string>(input_identifier);
    }
    catch (const std::exception &) {
      params.set_output(socket_name, float3(0.0f));
      continue;
    }

    if (bone_name.empty()) {
      params.set_output(socket_name, float3(0.0f));
      continue;
    }

    bPoseChannel *pchan = BKE_pose_channel_find_name(object_eval->pose, bone_name.c_str());
    if (!pchan) {
      continue;
    }

    float4x4 mat(pchan->pose_mat);
    float3 pos = mat.location();

    params.set_output(socket_name, pos);
  }
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNode &node = *static_cast<bNode *>(ptr->data);
  NodeGeometryArmatureInfo &storage = node_storage(node);
  if (uiLayout *panel = layout->panel(C, "armature_info_items", false, IFACE_("Items"))) {
    panel->op("node.armature_info_item_add", IFACE_("Add Item"), ICON_ADD);
    uiLayout *col = &panel->column(false);
    for (const int i : IndexRange(storage.items_num)) {
      uiLayout *row = &col->row(false);
      row->label(node.input_socket(i + 1).name, ICON_NONE);
      row->label(node.output_socket(i + 1).name, ICON_NONE);
      PointerRNA op_ptr = row->op("node.armature_info_item_remove", "", ICON_REMOVE);
      RNA_int_set(&op_ptr, "index", i);
    }
  }
}

static void NODE_OT_armature_info_item_add(wmOperatorType *ot)
{
  socket_items::ops::add_item<ArmatureInfoItemsAccessor>(ot, "Add Item", __func__, "Add bake item");
}

static void NODE_OT_armature_info_item_remove(wmOperatorType *ot)
{
  socket_items::ops::remove_item_by_index<ArmatureInfoItemsAccessor>(
      ot, "Remove Item", __func__, "Remove an item from the armature info");
}

static void node_operators()
{
  WM_operatortype_append(NODE_OT_armature_info_item_add);
  WM_operatortype_append(NODE_OT_armature_info_item_remove);
}

static void node_node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryArmatureInfo *data = MEM_callocN<NodeGeometryArmatureInfo>(__func__);
  data->transform_space = GEO_NODE_TRANSFORM_SPACE_ORIGINAL;
  node->storage = data;

  data->data_type = SOCK_STRING;
  data->next_identifier = 0;

  BLI_assert(data->items == nullptr);
  const int default_items_num = 2;
  data->items = MEM_calloc_arrayN<ArmatureInfoItem>(default_items_num, __func__);
  for (const int i : IndexRange(default_items_num)) {
    data->items[i].identifier = data->next_identifier++;
  }
  data->items_num = default_items_num;
  node->storage = data;
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeArmatureInfo", GEO_NODE_ARMATURE_INFO);
  ntype.ui_name = "Armature Info";
  ntype.ui_description = "Check if an object is an armature";
  ntype.enum_name_legacy = "ARMATURE_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = node_node_init;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryArmatureInfo", node_free_standard_storage, node_copy_standard_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.updatefunc = node_update;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_armature_info_cc

blender::Span<ArmatureInfoItem> NodeGeometryArmatureInfo::items_span() const
{
  return blender::Span<ArmatureInfoItem>(items, items_num);
}

blender::MutableSpan<ArmatureInfoItem> NodeGeometryArmatureInfo::items_span()
{
  return blender::MutableSpan<ArmatureInfoItem>(items, items_num);
}