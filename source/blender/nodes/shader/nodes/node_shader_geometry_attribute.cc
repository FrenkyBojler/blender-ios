/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "node_util.hh"

#include "BLI_cache_mutex.hh"

#include "BKE_node_tree_interface.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_lib_id.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_context.hh"
#include "BKE_compute_context_cache.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_caller_ui.hh"
#include "NOD_geometry_nodes_srna.hh"
#include "NOD_geometry_nodes_log.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_socket_usage_inference.hh"

#include "ED_node.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "wm_event_system.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph_query.hh"

namespace blender {

namespace nodes::node_shader_geometry_attribute_cc {

static void interface_socket_declaration(const bNodeTreeInterfaceSocket &io_socket,
                                         const SocketUsageInferenceFn &usafe_fn,
                                         DeclarationListBuilder &b)
{
  bke::bNodeSocketType *base_typeinfo = bke::node_socket_type_find(io_socket.socket_type);
  eNodeSocketDatatype datatype = SOCK_CUSTOM;

  const UString name(io_socket.name);
  const UString identifier(io_socket.identifier);

  BaseSocketDeclarationBuilder *decl = nullptr;
  if (base_typeinfo) {
    datatype = base_typeinfo->type;
    switch (datatype) {
      case SOCK_FLOAT: {
        const auto &value = bke::node_interface::get_socket_data_as<bNodeSocketValueFloat>(io_socket);
        decl = &b.add_output<decl::Float>(name, identifier).subtype(PropertySubType(value.subtype));
        break;
      }
      case SOCK_VECTOR: {
        const auto &value = bke::node_interface::get_socket_data_as<bNodeSocketValueVector>(io_socket);
        decl = &b.add_output<decl::Vector>(name, identifier).subtype(PropertySubType(value.subtype));
        break;
      }
      case SOCK_INT: {
        const auto &value = bke::node_interface::get_socket_data_as<bNodeSocketValueInt>(io_socket);
        decl = &b.add_output<decl::Int>(name, identifier).subtype(PropertySubType(value.subtype));
        break;
      }
      case SOCK_STRING: {
        const auto &value = bke::node_interface::get_socket_data_as<bNodeSocketValueString>(io_socket);
        decl = &b.add_output<decl::String>(name, identifier).subtype(PropertySubType(value.subtype));
        break;
      }
      case SOCK_RGBA: {
        decl = &b.add_output<decl::Color>(name, identifier);
        break;
      }
      case SOCK_BOOLEAN: {
        decl = &b.add_output<decl::Bool>(name, identifier);
        break;
      }
      default:
        break;
    }
  }
  else {
    decl = &b.add_output<decl::Custom>(name, identifier).idname(io_socket.socket_type);
  }

  if (decl == nullptr) {
    return;
  }

  decl->description(io_socket.description ? io_socket.description : "");
  decl->panel_toggle(io_socket.flag & NODE_INTERFACE_SOCKET_PANEL_TOGGLE);
  decl->optional_label(io_socket.flag & NODE_INTERFACE_SOCKET_OPTIONAL_LABEL);
  decl->usage_inference(usafe_fn);
}

static void declare_panel_recursive(DeclarationListBuilder &b,
                                    const SocketUsageInferenceFn &usafe_fn,
                                    const bNodeTreeInterfacePanel &io_parent_panel)
{
  for (const bNodeTreeInterfaceItem *item : io_parent_panel.items()) {
    switch (eNodeTreeInterfaceItemType(item->item_type)) {
      case NODE_INTERFACE_SOCKET: {
        const auto &io_socket = bke::node_interface::get_item_as<bNodeTreeInterfaceSocket>(*item);
        if (io_socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
          continue;
        }
        interface_socket_declaration(io_socket, usafe_fn, b);
        break;
      }
      case NODE_INTERFACE_PANEL: {
        const auto &io_panel = bke::node_interface::get_item_as<bNodeTreeInterfacePanel>(*item);
        auto &panel_b = b.add_panel(UString(io_panel.name), io_panel.identifier)
                            .description(StringRef(io_panel.description))
                            .default_closed(io_panel.flag & NODE_INTERFACE_PANEL_DEFAULT_CLOSED);
        declare_panel_recursive(panel_b, usafe_fn, io_panel);
        break;
      }
    }
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (node == nullptr) {
    return;
  }
  
  const bNodeTree *tree = b.tree_or_null();
  if (tree == nullptr) {
    return;
  }

  NodeDeclaration &r_declaration = b.declaration();
  const bNodeTree *group = reinterpret_cast<const bNodeTree *>(node->id);
  if (!group) {
    return;
  }
  if (ID_IS_LINKED(&group->id)) {
    if (ID_MISSING(&group->id)) {
      r_declaration.skip_updating_sockets = true;
      return;
    }
    /* Currently the missing flag is only set on original data. */
    if (const ID *orig_group = DEG_get_original_id(&group->id)) {
      if (ID_MISSING(orig_group)) {
        r_declaration.skip_updating_sockets = true;
        return;
      }
    }
  }
  r_declaration.skip_updating_sockets = false;

  /* Allow the node group interface to define the socket order. */
  r_declaration.use_custom_socket_order = true;

  group->ensure_interface_cache();

  b.add_default_layout();

  auto output_is_enabled = [tree, node, group](const int socket_i) -> bool {
    PointerRNA node_ptr = RNA_pointer_create_discrete(const_cast<ID *>(&tree->id), RNA_ShaderNodeGeometryAttribute, const_cast<bNode *>(node));
    PointerRNA properties_ptr = RNA_pointer_get(&node_ptr, "properties");

    ResourceScope scope;
    Vector<InferenceValue> input_values = nodes::get_geometry_nodes_input_inference_values(*group, properties_ptr, scope);
    const auto get_input_value = [&](const int group_input_i) {
      return input_values[group_input_i];
    };

    bke::ComputeContextCache compute_context_cache;
    SocketValueInferencer value_inferencer(*group, scope, compute_context_cache, get_input_value);
    socket_usage_inference::SocketUsageInferencer usage_inferencer(*group, scope, value_inferencer, compute_context_cache);

    return !usage_inferencer.is_disabled_group_output(socket_i);
  };

  declare_panel_recursive(b,
                          [group, output_is_enabled](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
                            params.tree.ensure_topology_cache();
                            const int socket_i = params.socket.index();
                            return output_is_enabled(socket_i);;
                          },
                          group->tree_interface.root_panel);
}

struct SocketSearchData {
};

struct DrawGroupInputsContext {
  const bContext &C;
  bNodeTree *tree;
  geo_eval_log::GeoTreeLog *tree_log;
  PointerRNA *properties_ptr;
  PointerRNA *bmain_ptr;
  Array<nodes::socket_usage_inference::SocketUsage> input_usages;
  Array<nodes::socket_usage_inference::SocketUsage> output_usages;
  bool use_name_for_ids = false;
  std::function<SocketSearchData(const bNodeTreeInterfaceSocket &)> socket_search_data_fn;
  std::function<void(ui::Layout &, int icon, const bNodeTreeInterfaceSocket &)>
      draw_attribute_toggle_fn;

  bool input_is_visible(const bNodeTreeInterfaceSocket &socket) const
  {
    return true;
    // return this->input_usages[this->tree->interface_input_index(socket)].is_visible;
  }

  bool input_is_active(const bNodeTreeInterfaceSocket &socket) const
  {
    return true;
    // return this->input_usages[this->tree->interface_input_index(socket)].is_used;
  }
};

static void add_layer_name_search_button(DrawGroupInputsContext &ctx,
                                         ui::Layout &layout,
                                         const bNodeTreeInterfaceSocket &socket,
                                         PointerRNA *socket_props_ptr)
{
  if (!ctx.tree_log) {
    layout.prop(socket_props_ptr, "layer_name", UI_ITEM_NONE, "", ICON_NONE);
    return;
  }

  layout.use_property_decorate_set(false);

  ui::Layout &split = layout.split(0.4f, false);
  ui::Layout &name_row = split.row(false);
  name_row.alignment_set(ui::LayoutAlign::Right);

  name_row.label(socket.name ? IFACE_(socket.name) : "", ICON_NONE);
  ui::Layout &prop_row = split.row(true);

  ui::Block *block = prop_row.block();
  ui::Button *but = uiDefIconTextButR(block,
                                      ui::ButtonType::SearchMenu,
                                      ICON_OUTLINER_DATA_GP_LAYER,
                                      "",
                                      0,
                                      0,
                                      10 * UI_UNIT_X, /* Dummy value, replaced by layout system. */
                                      UI_UNIT_Y,
                                      socket_props_ptr,
                                      "layer_name",
                                      0,
                                      StringRef(socket.description));
  button_placeholder_set(but, IFACE_("Layer"));
  layout.label("", ICON_BLANK1);

  const Object *object = ed::object::context_object(&ctx.C);
  BLI_assert(object != nullptr);
  if (object == nullptr) {
    return;
  }
}

static void attribute_search_update_fn(
    const bContext *C, void *arg, const char *str, ui::SearchItems *items, const bool is_first)
{/*
  SocketSearchData &data = *static_cast<SocketSearchData *>(arg);
  const SearchInfo info = data.info(*C);
  if (!info.tree || !info.tree_log) {
    return;
  }
  info.tree_log->ensure_existing_attributes();
  info.tree->ensure_topology_cache();

  Vector<const bNodeSocket *> sockets_to_check;
  if (data.is_output) {
    for (const bNode *node : info.tree->nodes_by_type("NodeGroupOutput"_ustr)) {
      for (const bNodeSocket *socket : node->input_sockets()) {
        if (socket->type == SOCK_GEOMETRY) {
          sockets_to_check.append(socket);
        }
      }
    }
  }
  else {
    for (const bNode *node : info.tree->group_input_nodes()) {
      for (const bNodeSocket *socket : node->output_sockets()) {
        if (socket->type == SOCK_GEOMETRY) {
          sockets_to_check.append(socket);
        }
      }
    }
  }
  Set<StringRef> names;
  Vector<const geo_eval_log::GeometryAttributeInfo *> attributes;
  for (const bNodeSocket *socket : sockets_to_check) {
    const geo_eval_log::ValueLog *value_log = info.tree_log->find_socket_value_log(*socket);
    if (value_log == nullptr) {
      continue;
    }
    if (const auto *geo_log = dynamic_cast<const geo_eval_log::GeometryInfoLog *>(value_log)) {
      for (const geo_eval_log::GeometryAttributeInfo &attribute : geo_log->attributes) {
        if (names.add(attribute.name)) {
          attributes.append(&attribute);
        }
      }
    }
  }
  ui::attribute_search_add_items(str, data.is_output, attributes.as_span(), items, is_first);*/
}

static void attribute_search_exec_fn(bContext *C, void *data_v, void *item_v)
{
  if (item_v == nullptr) {
    return;
  }
  SocketSearchData &data = *static_cast<SocketSearchData *>(data_v);
  const auto &item = *static_cast<const geo_eval_log::GeometryAttributeInfo *>(item_v);
  // SearchInfo info = data.info(*C);
  // if (!info.socket_props_ptr) {
  //   return;
  // }
  // 
  // RNA_string_set(&*info.socket_props_ptr, "attribute_name", item.name.c_str());
  ED_undo_push(C, "Assign Attribute Name");
}

static void add_attribute_search_button(DrawGroupInputsContext &ctx,
                                        ui::Layout &layout,
                                        PointerRNA *socket_props_ptr,
                                        const bNodeTreeInterfaceSocket &socket)
{
  if (!ctx.tree_log) {
    layout.prop(socket_props_ptr, "attribute_name", UI_ITEM_NONE, "", ICON_NONE);
    return;
  }

  ui::Block *block = layout.block();
  ui::Button *but = uiDefIconTextButR(block,
                                      ui::ButtonType::SearchMenu,
                                      ICON_NONE,
                                      "",
                                      0,
                                      0,
                                      10 * UI_UNIT_X, /* Dummy value, replaced by layout system. */
                                      UI_UNIT_Y,
                                      socket_props_ptr,
                                      "attribute_name",
                                      0,
                                      StringRef(socket.description));

  const Object *object = ed::object::context_object(&ctx.C);
  BLI_assert(object != nullptr);
  if (object == nullptr) {
    return;
  }

  /* Using a custom free function make the search not work currently. So make sure this data can be
   * freed with MEM_delete. */
  SocketSearchData *data = static_cast<SocketSearchData *>(
      MEM_new_uninitialized(sizeof(SocketSearchData), __func__));
  *data = ctx.socket_search_data_fn(socket);
  button_func_search_set_results_are_suggestions(but, true);
  button_func_search_set_sep_string(but, UI_MENU_ARROW_SEP);
  button_func_search_set(but,
                         nullptr,
                         attribute_search_update_fn,
                         data,
                         true,
                         nullptr,
                         attribute_search_exec_fn,
                         nullptr);

  std::string attribute_name = RNA_string_get(socket_props_ptr, "attribute_name");
  const bool access_allowed = bke::allow_procedural_attribute_access(attribute_name);
  if (!access_allowed) {
    button_flag_enable(but, ui::BUT_REDALERT);
  }
}

static void add_attribute_search_or_value_buttons(
    DrawGroupInputsContext &ctx,
    ui::Layout &layout,
    const bNodeTreeInterfaceSocket &socket,
    PointerRNA *socket_props_ptr,
    const std::optional<StringRefNull> use_name = std::nullopt)
{
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  const eNodeSocketDatatype type = typeinfo ? typeinfo->type : SOCK_CUSTOM;

  const bool show_attribute_input = RNA_enum_get(socket_props_ptr, "type") ==
                                    int(nodes::GeometryNodesInputType::Attribute);

  /* We're handling this manually in this case. */
  layout.use_property_decorate_set(false);

  ui::Layout &split = layout.split(0.4f, false);
  ui::Layout &name_row = split.row(false);
  name_row.alignment_set(ui::LayoutAlign::Right);

  ui::Layout *prop_row = nullptr;
  const StringRefNull socket_name = use_name.has_value() ?
                                        (*use_name) :
                                        (socket.name ? IFACE_(socket.name) : "");

  if (type == SOCK_BOOLEAN && !show_attribute_input) {
    name_row.label("", ICON_NONE);
    prop_row = &split.row(true);
  }
  else {
    prop_row = &layout.row(true);
  }

  if (type == SOCK_BOOLEAN) {
    prop_row->use_property_split_set(false);
    prop_row->alignment_set(ui::LayoutAlign::Expand);
  }

  if (show_attribute_input) {
    name_row.label(IFACE_(socket_name), ICON_NONE);
    prop_row = &split.row(true);
    add_attribute_search_button(ctx, *prop_row, socket_props_ptr, socket);
    layout.label("", ICON_BLANK1);
  }
  else {
    const char *name = IFACE_(socket_name.c_str());
    prop_row->prop(socket_props_ptr, "value", UI_ITEM_NONE, name, ICON_NONE);
    layout.decorator(socket_props_ptr, "value", -1);
  }

  ctx.draw_attribute_toggle_fn(*prop_row, ICON_SPREADSHEET, socket);
}

/* Drawing the properties manually with #ui::Layout::prop instead of #uiDefAutoButsRNA allows using
 * the node socket identifier for the property names, since they are unique, but also having
 * the correct label displayed in the UI. */
static void draw_property_for_socket(DrawGroupInputsContext &ctx,
                                     ui::Layout &layout,
                                     const bNodeTreeInterfaceSocket &socket,
                                     PointerRNA *socket_props_ptr,
                                     const std::optional<StringRef> parent_name = std::nullopt)
{
  const int input_index = ctx.tree->interface_input_index(socket);
  if (!ctx.input_is_visible(socket)) {
    /* The input is not used currently, but it would be used if any menu input is changed.
     * By convention, the input is hidden in this case instead of just grayed out. */
    return;
  }

  ui::Layout &row = layout.row(true);
  row.use_property_decorate_set(true);
  row.active_set(ctx.input_is_active(socket));

  /* Use #ui::Layout::prop_search to draw pointer properties because #ui::Layout::prop would not
   * have enough information about what type of ID to select for editing the values. This is
   * because pointer IDProperties contain no information about their type. */
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  const eNodeSocketDatatype type = typeinfo ? typeinfo->type : SOCK_CUSTOM;

  if (ELEM(type, SOCK_GEOMETRY, SOCK_MATRIX, SOCK_BUNDLE, SOCK_CLOSURE)) {
    return;
  }

  std::string name = socket.name ? IFACE_(socket.name) : "";

  /* If the property has a prefix that's the same string as the name of the panel it's in, remove
   * the prefix so it appears less verbose. */
  if (parent_name.has_value()) {
    const StringRef prefix_to_remove = *parent_name;
    const int prefix_size = prefix_to_remove.size();
    const int pos = name.find(prefix_to_remove);
    if (pos == 0 && name.size() > prefix_size && name[prefix_size] == ' ') {
      name = name.substr(prefix_size + 1);
    }
  }

  switch (type) {
    case SOCK_OBJECT: {
      row.prop_search(socket_props_ptr, "value", ctx.bmain_ptr, "objects", name, ICON_OBJECT_DATA);
      break;
    }
    case SOCK_COLLECTION: {
      row.prop_search(
          socket_props_ptr, "value", ctx.bmain_ptr, "collections", name, ICON_OUTLINER_COLLECTION);
      break;
    }
    case SOCK_MATERIAL: {
      row.prop_search(socket_props_ptr, "value", ctx.bmain_ptr, "materials", name, ICON_MATERIAL);
      break;
    }
    case SOCK_TEXTURE: {
      row.prop_search(socket_props_ptr, "value", ctx.bmain_ptr, "textures", name, ICON_TEXTURE);
      break;
    }
    case SOCK_FONT: {
      PropertyRNA *prop = RNA_struct_find_property(ctx.properties_ptr, "value");
      if (prop && RNA_property_type(prop) == PROP_POINTER) {
        template_id(&row,
                    &ctx.C,
                    ctx.properties_ptr,
                    "value",
                    nullptr,
                    "FONT_OT_open",
                    "FONT_OT_unlink",
                    ui::TEMPLATE_ID_FILTER_ALL,
                    false,
                    name);
      }
      else {
        /* #template_id only supports pointer properties currently. Node tools store
         * data-block pointers in strings currently. */
        row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "fonts", name, ICON_FONT_DATA);
      }
      break;
    }
    case SOCK_SCENE: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "scenes", name, ICON_SCENE);
      break;
    }
    case SOCK_TEXT_ID: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "texts", name, ICON_TEXT);
      break;
    }
    case SOCK_MASK: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "masks", name, ICON_NONE);
      break;
    }
    case SOCK_SOUND: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "sounds", name, ICON_SOUND);
      break;
    }
    case SOCK_IMAGE: {
      template_id(&row,
                  &ctx.C,
                  socket_props_ptr,
                  "value",
                  "image.new",
                  "image.open",
                  nullptr,
                  ui::TEMPLATE_ID_FILTER_ALL,
                  false,
                  name);
      break;
    }
    case SOCK_MENU: {
      if (socket.flag & NODE_INTERFACE_SOCKET_MENU_EXPANDED) {
        /* Use a single space when the name is empty to work around a bug with expanded enums. Also
         * see #ui_item_enum_expand_exec. */
        row.prop(socket_props_ptr,
                 "value",
                 ui::ITEM_R_EXPAND,
                 StringRef(name).is_empty() ? " " : name,
                 ICON_NONE);
      }
      else {
        row.prop(socket_props_ptr, "value", UI_ITEM_NONE, name, ICON_NONE);
      }
      break;
    }
    case SOCK_BOOLEAN: {
      ATTR_FALLTHROUGH;
    }
    default: {
      if (nodes::input_has_attribute_toggle(*ctx.tree, input_index)) {
        add_attribute_search_or_value_buttons(ctx, row, socket, socket_props_ptr, name);
      }
      else {
        row.prop(socket_props_ptr, "value", UI_ITEM_NONE, name, ICON_NONE);
      }
      break;
    }
  }
  if (!nodes::input_has_attribute_toggle(*ctx.tree, input_index)) {
    row.label("", ICON_BLANK1);
  }
}

void draw_geometry_nodes_modifier_ui(const bContext &C,
                                     PointerRNA *node_ptr,
                                     ui::Layout &layout)
{
  Main *bmain = CTX_data_main(&C);
  PointerRNA bmain_ptr = RNA_main_pointer_create(bmain);
  bNode &node = *node_ptr->data_as<bNode>();
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(node_ptr->owner_id);
  PointerRNA properties_ptr = RNA_pointer_get(node_ptr, "properties");

  bNodeTree *group_tree = id_cast<bNodeTree *>(node.id);
  DrawGroupInputsContext ctx{C, group_tree, nullptr, &properties_ptr, &bmain_ptr};

  ctx.socket_search_data_fn = [&](const bNodeTreeInterfaceSocket &io_socket) -> SocketSearchData {
    return {};
  };
  ctx.draw_attribute_toggle_fn =
      [&](ui::Layout &layout, const int icon, const bNodeTreeInterfaceSocket &io_socket) {
        // PointerRNA props = layout.op("node.shader_geometry_node_input_attribute_toggle",
        //                              "",
        //                              icon,
        //                              wm::OpCallContext::InvokeDefault,
        //                              UI_ITEM_NONE);
        // RNA_string_set(&props, "node_name", node.name);
        // RNA_string_set(&props, "input_name", io_socket.identifier);
      };

  layout.use_property_split_set(true);
  /* Decorators are added manually for supported properties because the
   * attribute/value toggle requires a manually built layout anyway. */
  layout.use_property_decorate_set(false);

  template_id(&layout, &C, node_ptr, "node_tree", nullptr, nullptr, nullptr);

  if (group_tree == nullptr || ID_MISSING(group_tree)) {
    return;
  }
  draw_interface_panel_content(
      C,
      layout,
      &properties_ptr,
      group_tree->tree_interface.root_panel,
      [&](const bNodeTreeInterfaceSocket &socket) { return ctx.input_is_visible(socket); },
      [&](const bNodeTreeInterfaceSocket &socket) { return ctx.input_is_active(socket); },
      [&](ui::Layout &layout,
          const bNodeTreeInterfaceSocket &socket,
          PointerRNA *socket_props_ptr,
          const std::optional<StringRef> parent_name) {
        draw_property_for_socket(ctx, layout, socket, socket_props_ptr, parent_name);
      });
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  // layout.use_property_split_set(true);
  // layout.use_property_decorate_set(false);
  // layout.prop(ptr, "node_tree", UI_ITEM_NONE, "", ICON_NONE);
  
  draw_geometry_nodes_modifier_ui(*C, ptr, layout);
}

static int node_shader_gpu_attribute(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  const bNodeTree *node_group = id_cast<const bNodeTree *>(node->id);
  
  if (node_group == nullptr) {
    return 0;
  }

  int output_index = 0;
  
  const std::string capture_prefix = std::string("._a_capture[") + BKE_id_name(node_group->id) + "]";
  node_group->tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
    if (eNodeTreeInterfaceItemType(item.item_type) != NODE_INTERFACE_SOCKET) {
      return true;
    }

    const bNodeTreeInterfaceSocket &socket = bke::node_interface::get_item_as<bNodeTreeInterfaceSocket>(item);
    if (socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
      return true;
    }
    
    const bke::bNodeSocketType *type = socket.socket_typeinfo();
    if (type == nullptr) {
      return true;
    }
    if (!ELEM(type->idname, "NodeSocketFloat", "NodeSocketInt", "NodeSocketColor", "NodeSocketBool", "NodeSocketVector")) {
      return true;
    }
    
    const std::string capture_name = capture_prefix + "[" + socket.identifier + "]";

    const auto func_name = [&]() -> StringRefNull {
      if (type->idname == "NodeSocketFloat") {
        return "node_attribute_as_float";
      }
      if (type->idname == "NodeSocketVector") {
        return "node_attribute_as_float3";
      }
      if (type->idname == "NodeSocketInt") {
        return "node_attribute_as_int";
      }
      if (type->idname == "NodeSocketColor") {
        BLI_assert(false);
        return "node_attribute_as_int";
      }
      if (type->idname == "NodeSocketBool") {
        return "node_attribute_as_bool";
      }
      BLI_assert(false);
      return "";
    }();

    // printf("Bind to\"%s\";\n", capture_name.c_str());
    GPUNodeLink *attribute = GPU_attribute(mat, CD_AUTO_FROM_NAME, capture_name.c_str());
    GPU_link(mat, func_name.c_str(), attribute, &out[output_index].link);
    output_index++;
    
    return true;
  });

  return 1;
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
  node->id = nullptr;
}

}  // namespace nodes::node_shader_geometry_attribute_cc

/* node type definition */
void register_node_type_sh_geometry_attribute()
{
  namespace file_ns = nodes::node_shader_geometry_attribute_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeGeometryAttribute"_ustr);
  ntype.ui_name = "Geometry Attribute";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = file_ns::node_init;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.gpu_fn = file_ns::node_shader_gpu_attribute;

  bke::node_register_type(ntype);
}

}  // namespace blender
