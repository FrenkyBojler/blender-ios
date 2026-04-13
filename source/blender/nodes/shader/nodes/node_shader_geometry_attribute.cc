/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "node_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "BKE_node_tree_interface.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_lib_id.hh"

#include "RNA_access.hh"

#include "DEG_depsgraph_query.hh"

namespace blender {

namespace nodes::node_shader_geometry_attribute_cc {

static void interface_socket_declaration(const bNodeTreeInterfaceSocket &io_socket, DeclarationListBuilder &b)
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
}

static void declare_panel_recursive(DeclarationListBuilder &b, const bNodeTreeInterfacePanel &io_parent_panel)
{
  for (const bNodeTreeInterfaceItem *item : io_parent_panel.items()) {
    switch (eNodeTreeInterfaceItemType(item->item_type)) {
      case NODE_INTERFACE_SOCKET: {
        const auto &io_socket = bke::node_interface::get_item_as<bNodeTreeInterfaceSocket>(*item);
        if (io_socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
          continue;
        }
        interface_socket_declaration(io_socket, b);
        break;
      }
      case NODE_INTERFACE_PANEL: {
        const auto &io_panel = bke::node_interface::get_item_as<bNodeTreeInterfacePanel>(*item);
        auto &panel_b = b.add_panel(UString(io_panel.name), io_panel.identifier)
                            .description(StringRef(io_panel.description))
                            .default_closed(io_panel.flag & NODE_INTERFACE_PANEL_DEFAULT_CLOSED);
        declare_panel_recursive(panel_b, io_panel);
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

  declare_panel_recursive(b, group->tree_interface.root_panel);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "node_tree", UI_ITEM_NONE, "", ICON_NONE);
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
  
  printf("\n");
  
  int output_index = 0;
  
  const std::string capture_prefix = std::string(".capture[") + BKE_id_name(node_group->id) + "]";
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
    if (!ELEM(type->idname, "NodeSocketFloat", "NodeSocketInt", "NodeSocketColor", "NodeSocketBool")) {
      return true;
    }
    
    const std::string capture_name = capture_prefix + "." + socket.identifier;

    const auto func_name = [&]() -> StringRefNull {
      if (type->idname == "NodeSocketFloat") {
        return "node_attribute_as_float";
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

    printf("Bind to\"%s\";\n", capture_name.c_str());
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
