/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"

#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rand.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"

#include "ANIM_action.hh"

#include "DEG_depsgraph_build.hh"

#include "ED_node.hh"
#include "ED_node_preview.hh"
#include "ED_render.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_resources.hh"

#include "NOD_common.hh"
#include "NOD_composite.hh"
#include "NOD_geometry.hh"
#include "NOD_node_declaration.hh"
#include "NOD_shader.h"
#include "NOD_socket.hh"
#include "NOD_texture.h"

#include "node_intern.hh" /* own include */

namespace blender {

/* Utility for referencing a const socket and its owner node. */
struct NodeSocketRef {
  const bNode &node;
  const bNodeSocket &socket;

  friend bool operator==(const NodeSocketRef &a, const NodeSocketRef &b)
  {
    return (&a.node == &b.node) && (&a.socket == &b.socket);
  }
  BLI_STRUCT_DERIVED_UNEQUAL_OPERATOR(NodeSocketRef)
};

/* Utility for referencing a mutable socket and its owner node. */
struct MutableNodeSocketRef {
  bNode &node;
  bNodeSocket &socket;

  NodeSocketRef operator()() const
  {
    return {node, socket};
  }

  friend bool operator==(const MutableNodeSocketRef &a, const MutableNodeSocketRef &b)
  {
    return (&a.node == &b.node) && (&a.socket == &b.socket);
  }
  BLI_STRUCT_DERIVED_UNEQUAL_OPERATOR(MutableNodeSocketRef)
};

template<> struct DefaultHash<NodeSocketRef> {
  uint64_t operator()(const NodeSocketRef &value) const
  {
    return get_default_hash(&value.socket);
  }
  uint64_t operator()(const bNodeSocket &socket) const
  {
    return get_default_hash(&socket);
  }
};

template<> struct DefaultHash<MutableNodeSocketRef> {
  uint64_t operator()(const MutableNodeSocketRef &value) const
  {
    return get_default_hash(&value.socket);
  }
  uint64_t operator()(const bNodeSocket &socket) const
  {
    return get_default_hash(&socket);
  }
};

}  // namespace blender

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

static bool node_group_operator_active_poll(bContext *C)
{
  if (ED_operator_node_active(C)) {
    SpaceNode *snode = CTX_wm_space_node(C);

    /* Group operators only defined for standard node tree types.
     * Disabled otherwise to allow python-nodes define their own operators
     * with same key-map. */
    if (STR_ELEM(snode->tree_idname,
                 "ShaderNodeTree",
                 "CompositorNodeTree",
                 "TextureNodeTree",
                 "GeometryNodeTree"))
    {
      return true;
    }
  }
  return false;
}

static bool node_group_operator_editable(bContext *C)
{
  if (ED_operator_node_editable(C)) {
    SpaceNode *snode = CTX_wm_space_node(C);

    /* Group operators only defined for standard node tree types.
     * Disabled otherwise to allow python-nodes define their own operators
     * with same key-map. */
    if (ED_node_is_shader(snode) || ED_node_is_compositor(snode) || ED_node_is_texture(snode) ||
        ED_node_is_geometry(snode))
    {
      return true;
    }
  }
  return false;
}

static StringRef group_ntree_idname(bContext *C)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  return snode->tree_idname;
}

StringRef node_group_idname(const bContext *C)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  if (ED_node_is_shader(snode)) {
    return ntreeType_Shader->group_idname;
  }
  if (ED_node_is_compositor(snode)) {
    return ntreeType_Composite->group_idname;
  }
  if (ED_node_is_texture(snode)) {
    return ntreeType_Texture->group_idname;
  }
  if (ED_node_is_geometry(snode)) {
    return ntreeType_Geometry->group_idname;
  }

  return "";
}

static bNode *node_group_get_active(bContext *C, const StringRef node_idname)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  bNode *node = bke::node_get_active(*snode->edittree);

  if (node && node->idname == node_idname) {
    return node;
  }
  return nullptr;
}

/* Maps old to new identifiers for simulation input node pairing. */
static void remap_pairing(bNodeTree &dst_tree,
                          Span<bNode *> nodes,
                          const Map<int32_t, int32_t> &identifier_map)
{
  for (bNode *dst_node : nodes) {
    if (bke::all_zone_input_node_types().contains(dst_node->type_legacy)) {
      const bke::bNodeZoneType &zone_type = *bke::zone_type_by_node_type(dst_node->type_legacy);
      int &output_node_id = zone_type.get_corresponding_output_id(*dst_node);
      if (output_node_id == 0) {
        continue;
      }
      output_node_id = identifier_map.lookup_default(output_node_id, 0);
      if (output_node_id == 0) {
        nodes::update_node_declaration_and_sockets(dst_tree, *dst_node);
      }
    }
  }
}

static std::string node_basepath(const bNodeTree &tree, const bNode &node)
{
  const PointerRNA ptr = RNA_pointer_create_discrete(
      &const_cast<bNodeTree &>(tree).id, &RNA_Node, &const_cast<bNode &>(node));
  return *RNA_path_from_ID_to_struct(&ptr);
}

template<typename ContainerT> std::optional<Bounds<float2>> node_bounds(const ContainerT &nodes)
{
  std::optional<Bounds<float2>> result = std::nullopt;
  for (const bNode *node : nodes) {
    const float2 loc(node->location);
    const Bounds<float2> box(loc, loc + float2(node->width, -node->height));
    result = bounds::merge<float2>(result, box);
  }
  return result;
}

template<typename ContainerT>
std::optional<Bounds<float2>> node_location_bounds(const ContainerT &nodes)
{
  std::optional<Bounds<float2>> result = std::nullopt;
  for (const bNode *node : nodes) {
    const float2 loc(node->location);
    result = bounds::min_max(result, loc);
  }
  return result;
}

namespace util {

using NodeFilterFn = FunctionRef<bool(const bNode &)>;
static bool default_link_filter(const bNode & /*node*/)
{
  return true;
}

/* Links between nodes that should be copied.
 * Contains only links between nodes of this set. */
static Vector<const bNodeLink *> find_internal_links(
    const bNodeTree &tree,
    const Span<const bNode *> nodes,
    NodeFilterFn link_filter = default_link_filter)
{
  Vector<const bNodeLink *> internal_links;
  const Set<const bNode *> nodes_set(nodes);
  for (const bNodeLink &link : tree.links) {
    if (!link.is_available() || bke::node_link_is_hidden(link)) {
      continue;
    }
    if (!link_filter(*link.fromnode) || !link_filter(*link.tonode)) {
      continue;
    }
    if (!nodes_set.contains(link.fromnode) || !nodes_set.contains(link.tonode)) {
      continue;
    }

    internal_links.append(&link);
  }
  return internal_links;
}

static Vector<MutableNodeSocketRef> get_external_links(
    const bNodeSocket &socket, NodeFilterFn link_filter = default_link_filter)
{
  Vector<MutableNodeSocketRef> result;
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (bke::node_link_is_hidden(*link)) {
      continue;
    }
    bNode *link_node = socket.is_input() ? link->fromnode : link->tonode;
    bNodeSocket *link_socket = socket.is_input() ? link->fromsock : link->tosock;
    if (!link_filter(*link_node)) {
      continue;
    }
    result.append({*link_node, *link_socket});
  }
  return result;
}

}  // namespace util

/**
 * Describes tree interface items generated from a node subset.
 */
class NodeSetInterface {
 public:
  struct InterfaceSocketData {
    VectorSet<NodeSocketRef> internal_sockets;
    VectorSet<MutableNodeSocketRef> external_sockets;
    bool hidden = false;
    bool collapsed = false;
  };
  struct InterfacePanelData {
    bool collapsed = false;
  };

 private:
  Map<const bNodeTreeInterfaceSocket *, InterfaceSocketData> socket_data_;
  Map<const bNodeTreeInterfacePanel *, InterfacePanelData> panel_data_;

 public:
  const Map<const bNodeTreeInterfaceSocket *, InterfaceSocketData> &socket_data() const;
  const Map<const bNodeTreeInterfacePanel *, InterfacePanelData> &panel_data() const;

  static NodeSetInterface from_nodes(const bNodeTree &tree,
                                     const Span<const bNode *> nodes,
                                     bNodeTree &dst_tree,
                                     const bool expose_visible);

  static NodeSetInterface from_node_declaration(const bNode &src_node, bNodeTree &dst_tree);

  /* Connect the group node to external sockets. */
  void connect_group_node(bNode &group_node) const;

 private:
  /**
   * Skip reroute nodes when finding the socket to use as an example for a new group interface
   * item. This moves "inward" into nodes selected for grouping to find properties like whether a
   * connected socket has a hidden value. It only works in trivial situations-- a single line of
   * connected reroutes with no branching.
   */
  static const bNodeSocket &find_socket_to_use_for_interface(const bNodeTree &node_tree,
                                                             const bNodeSocket &socket);

  static bNodeTreeInterfaceSocket *add_interface_from_socket(const bNodeTree &original_tree,
                                                             bNodeTree &tree_for_interface,
                                                             const bNodeSocket &socket);

  void add_declaration_item_recursive(bNodeTree &dst_tree,
                                      const bNode &src_node,
                                      const nodes::ItemDeclaration &item_decl,
                                      bNodeTreeInterfacePanel *parent);
};

const Map<const bNodeTreeInterfaceSocket *, NodeSetInterface::InterfaceSocketData> &
NodeSetInterface::socket_data() const
{
  return socket_data_;
}
const Map<const bNodeTreeInterfacePanel *, NodeSetInterface::InterfacePanelData> &
NodeSetInterface::panel_data() const
{
  return panel_data_;
}

NodeSetInterface NodeSetInterface::from_nodes(const bNodeTree &tree,
                                              const Span<const bNode *> nodes,
                                              bNodeTree &dst_tree,
                                              const bool expose_visible)
{
  NodeSetInterface result;
  /* Multiple internal or external sockets may be mapped to the same interface item. */
  Map<const bNodeSocket *, InterfaceSocketData *> data_by_socket;
  auto add_unique_interface = [&](const bNodeSocket &key,
                                  const bNodeSocket &template_socket,
                                  const NodeSocketRef &origin,
                                  const Span<MutableNodeSocketRef> links) {
    InterfaceSocketData &data = *data_by_socket.lookup_or_add_cb(&key, [&]() {
      const bNodeTreeInterfaceSocket *interface = add_interface_from_socket(
          tree, dst_tree, template_socket);
      BLI_assert(interface != nullptr);
      InterfaceSocketData &data = result.socket_data_.lookup_or_add(interface, {});
      return &data;
    });

    data.internal_sockets.add_new(origin);
    data.external_sockets.add_multiple(links);
  };

  tree.ensure_topology_cache();

  const Set<const bNode *> nodes_set(nodes);
  const std::function link_filter = [&](const bNode &link) { return !nodes_set.contains(&link); };
  for (const bNode *node : nodes) {
    for (const bNodeSocket *socket : node->input_sockets()) {
      if (!socket->is_available()) {
        continue;
      }

      const Vector<MutableNodeSocketRef> links = util::get_external_links(*socket, link_filter);
      const bool expose = !links.is_empty() || (expose_visible && socket->is_visible());
      if (!expose) {
        continue;
      }

      /* Inputs generate an interface socket for each unique input link. */
      const bNodeSocket &key = links.is_empty() ? *socket : links.first().socket;
      add_unique_interface(key, *socket, {*node, *socket}, links);
    }
    for (const bNodeSocket *socket : node->output_sockets()) {
      if (!socket->is_available()) {
        continue;
      }

      const Vector<MutableNodeSocketRef> links = util::get_external_links(*socket, link_filter);
      const bool expose = !links.is_empty() || (expose_visible && socket->is_visible());
      if (!expose) {
        continue;
      }

      /* Outputs generate an interface socket for each unique output link. */
      /* XXX This generates redundant sockets all based on the same internal socket.
       * It would make more sense to just create a single group socket just like the input
       * case.
       */
      const bNodeSocket &key = links.is_empty() ? *socket : links.first().socket;
      add_unique_interface(key, *socket, {*node, *socket}, links);
    }
  }
  return result;
}

NodeSetInterface NodeSetInterface::from_node_declaration(const bNode &src_node,
                                                         bNodeTree &dst_tree)
{
  BLI_assert(src_node.declaration() != nullptr);
  const nodes::NodeDeclaration &node_decl = *src_node.declaration();

  NodeSetInterface result;
  for (const nodes::ItemDeclaration *item_decl : node_decl.root_items) {
    result.add_declaration_item_recursive(dst_tree, src_node, *item_decl, nullptr);
  }

  return result;
}

/* Connect the group node to external sockets. */
void NodeSetInterface::connect_group_node(bNode &group_node) const
{
  bNodeTree &owner_tree = group_node.owner_tree();
  const bNodeTree &group_tree = *reinterpret_cast<bNodeTree *>(group_node.id);

  /* Cache node socket lists to avoid invalid topology cache after linking. */
  owner_tree.ensure_topology_cache();
  const Span<bNodeSocket *> group_node_inputs = group_node.input_sockets();
  const Span<bNodeSocket *> group_node_outputs = group_node.output_sockets();

  for (bNodeSocket *group_node_input : group_node_inputs) {
    const bNodeTreeInterfaceSocket *interface = bke::node_find_interface_input_by_identifier(
        group_tree, group_node_input->identifier);
    if (!interface) {
      continue;
    }
    const InterfaceSocketData *data = socket_data_.lookup_ptr(interface);
    BLI_assert(data);
    for (const MutableNodeSocketRef &link : data->external_sockets) {
      bke::node_add_link(owner_tree, link.node, link.socket, group_node, *group_node_input);
    }
    /* Keep old socket visibility. */
    SET_FLAG_FROM_TEST(group_node_input->flag, data->hidden, SOCK_HIDDEN);
    SET_FLAG_FROM_TEST(group_node_input->flag, data->collapsed, SOCK_COLLAPSED);
  }
  for (bNodeSocket *group_node_output : group_node_outputs) {
    const bNodeTreeInterfaceSocket *interface = bke::node_find_interface_output_by_identifier(
        group_tree, group_node_output->identifier);
    if (!interface) {
      continue;
    }
    const InterfaceSocketData *data = socket_data_.lookup_ptr(interface);
    BLI_assert(data);
    for (const MutableNodeSocketRef &link : data->external_sockets) {
      bke::node_add_link(owner_tree, group_node, *group_node_output, link.node, link.socket);
    }
    /* Keep old socket visibility. */
    SET_FLAG_FROM_TEST(group_node_output->flag, data->hidden, SOCK_HIDDEN);
    SET_FLAG_FROM_TEST(group_node_output->flag, data->collapsed, SOCK_COLLAPSED);
  }

  /* Keep old panel collapse status. */
  MutableSpan<bNodePanelState> panel_states = group_node.panel_states();
  for (const auto &item : panel_data_.items()) {
    for (bNodePanelState &new_panel_state : panel_states) {
      if (new_panel_state.identifier == item.key->identifier) {
        SET_FLAG_FROM_TEST(new_panel_state.flag, item.value.collapsed, NODE_PANEL_COLLAPSED);
      }
    }
  }
}

/**
 * Skip reroute nodes when finding the socket to use as an example for a new group interface
 * item. This moves "inward" into nodes selected for grouping to find properties like whether a
 * connected socket has a hidden value. It only works in trivial situations-- a single line of
 * connected reroutes with no branching.
 */
const bNodeSocket &NodeSetInterface::find_socket_to_use_for_interface(const bNodeTree &node_tree,
                                                                      const bNodeSocket &socket)
{
  if (node_tree.has_available_link_cycle()) {
    return socket;
  }
  const bNode &node = socket.owner_node();
  if (!node.is_reroute()) {
    return socket;
  }
  const bNodeSocket &other_socket = socket.in_out == SOCK_IN ? node.output_socket(0) :
                                                               node.input_socket(0);
  if (!other_socket.is_logically_linked()) {
    return socket;
  }
  return *other_socket.logically_linked_sockets().first();
}

bNodeTreeInterfaceSocket *NodeSetInterface::add_interface_from_socket(
    const bNodeTree &original_tree, bNodeTree &tree_for_interface, const bNodeSocket &socket)
{
  const bNode &node = socket.owner_node();
  /* The output sockets of group nodes usually have consciously given names so they have
   * precedence over socket names the link points to. */
  const bool prefer_node_for_interface_name = node.is_group() || node.is_group_input() ||
                                              node.is_group_output();

  /* The "example socket" has to have the same `in_out` status as the new interface socket. */
  const bNodeSocket &socket_for_io = find_socket_to_use_for_interface(original_tree, socket);
  const bNode &node_for_io = socket_for_io.owner_node();
  const bNodeSocket &socket_for_name = prefer_node_for_interface_name ? socket : socket_for_io;
  return bke::node_interface::add_interface_socket_from_node(
      tree_for_interface, node_for_io, socket_for_io, socket_for_io.idname, socket_for_name.name);
}

void NodeSetInterface::add_declaration_item_recursive(bNodeTree &dst_tree,
                                                      const bNode &src_node,
                                                      const nodes::ItemDeclaration &item_decl,
                                                      bNodeTreeInterfacePanel *parent)
{
  const Span<bNodePanelState> panel_states = src_node.panel_states();

  if (const nodes::SocketDeclaration *socket_decl = dynamic_cast<const nodes::SocketDeclaration *>(
          &item_decl))
  {
    const bNodeSocket &socket = src_node.socket_by_decl(*socket_decl);
    if (!socket.is_available()) {
      return;
    }
    bNodeTreeInterfaceSocket *io_socket = bke::node_interface::add_interface_socket_from_node(
        dst_tree, src_node, socket);
    if (io_socket) {
      dst_tree.tree_interface.move_item_to_parent(io_socket->item, parent, INT32_MAX);

      const NodeSocketRef origin = {src_node, socket};
      const bool hidden = socket.flag & SOCK_HIDDEN;
      const bool collapsed = socket.flag & SOCK_COLLAPSED;
      VectorSet<NodeSocketRef> internal_sockets = {origin};
      VectorSet<MutableNodeSocketRef> external_sockets = {util::get_external_links(socket)};
      socket_data_.add(
          io_socket,
          {std::move(internal_sockets), std::move(external_sockets), hidden, collapsed});
    }
  }
  else if (const nodes::PanelDeclaration *panel_decl =
               dynamic_cast<const nodes::PanelDeclaration *>(&item_decl))
  {
    NodeTreeInterfacePanelFlag flag{};
    if (panel_decl->default_collapsed) {
      flag |= NODE_INTERFACE_PANEL_DEFAULT_CLOSED;
    }
    bNodeTreeInterfacePanel *io_panel = dst_tree.tree_interface.add_panel(
        panel_decl->name, panel_decl->description, flag, parent);
    if (io_panel) {
      bool collapsed = false;
      for (const bNodePanelState panel_state : panel_states) {
        if (panel_state.identifier == panel_decl->identifier) {
          collapsed = panel_state.is_collapsed();
        }
      }
      panel_data_.add(io_panel, {collapsed});

      for (const nodes::ItemDeclaration *child_item_decl : panel_decl->items) {
        add_declaration_item_recursive(dst_tree, src_node, *child_item_decl, io_panel);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edit Group Operator
 * \{ */

static wmOperatorStatus node_group_edit_exec(bContext *C, wmOperator *op)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  ARegion *region = CTX_wm_region(C);
  const StringRef node_idname = node_group_idname(C);
  const bool exit = RNA_boolean_get(op->ptr, "exit");

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNode *gnode = node_group_get_active(C, node_idname);

  if (gnode && !exit) {
    bNodeTree *ngroup = id_cast<bNodeTree *>(gnode->id);

    if (ngroup) {
      ED_node_tree_push(region, snode, ngroup, gnode);
    }
  }
  else {
    ED_node_tree_pop(region, snode);
  }

  WM_event_add_notifier(C, NC_SCENE | ND_NODES, nullptr);
  WM_event_add_notifier(C, NC_NODE | ND_NODE_GIZMO, nullptr);

  return OPERATOR_FINISHED;
}

void NODE_OT_group_edit(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Edit Group";
  ot->description = "Edit node group";
  ot->idname = "NODE_OT_group_edit";

  /* API callbacks. */
  ot->exec = node_group_edit_exec;
  ot->poll = node_group_operator_active_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_boolean(ot->srna, "exit", false, "Exit", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Enter group at cursor, or exit when not hovering any node.
 * \{ */

static wmOperatorStatus node_group_enter_exit_invoke(bContext *C,
                                                     wmOperator * /*op*/,
                                                     const wmEvent *event)
{
  SpaceNode &snode = *CTX_wm_space_node(C);
  ARegion &region = *CTX_wm_region(C);

  /* Don't interfere when the mouse is interacting with some button. See #147282. */
  if (ISMOUSE_BUTTON(event->type) && ui::but_find_mouse_over(&region, event)) {
    return OPERATOR_PASS_THROUGH | OPERATOR_CANCELLED;
  }

  float2 cursor;
  ui::view2d_region_to_view(&region.v2d, event->mval[0], event->mval[1], &cursor.x, &cursor.y);
  bNode *node = node_under_mouse_get(snode, cursor);

  if (!node || node->is_frame()) {
    ED_node_tree_pop(&region, &snode);
    return OPERATOR_FINISHED;
  }
  if (!node->is_group()) {
    return OPERATOR_PASS_THROUGH;
  }
  if (node->is_custom_group()) {
    return OPERATOR_PASS_THROUGH;
  }
  bNodeTree *group = id_cast<bNodeTree *>(node->id);
  if (!group || ID_MISSING(group)) {
    return OPERATOR_PASS_THROUGH;
  }
  ED_node_tree_push(&region, &snode, group, node);
  return OPERATOR_FINISHED;
}

void NODE_OT_group_enter_exit(wmOperatorType *ot)
{
  ot->name = "Enter/Exit Group";
  ot->description = "Enter or exit node group based on cursor location";
  ot->idname = "NODE_OT_group_enter_exit";

  ot->invoke = node_group_enter_exit_invoke;
  ot->poll = node_group_operator_active_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ungroup Operator
 * \{ */

static void update_nested_node_refs_after_ungroup(bNodeTree &ntree,
                                                  const bNodeTree &ngroup,
                                                  const bNode &gnode,
                                                  const Map<int32_t, int32_t> &node_identifier_map)
{
  for (bNestedNodeRef &ref : ntree.nested_node_refs_span()) {
    if (ref.path.node_id != gnode.identifier) {
      continue;
    }
    const bNestedNodeRef *child_ref = ngroup.find_nested_node_ref(ref.path.id_in_node);
    if (!child_ref) {
      continue;
    }
    constexpr int32_t missing_id = -1;
    const int32_t new_node_id = node_identifier_map.lookup_default(child_ref->path.node_id,
                                                                   missing_id);
    if (new_node_id == missing_id) {
      continue;
    }
    ref.path.node_id = new_node_id;
    ref.path.id_in_node = child_ref->path.id_in_node;
  }
}

/**
 * \return True if successful.
 */
static void node_group_ungroup(Main *bmain, bNodeTree *ntree, bNode *gnode)
{
  Vector<AnimationBasePathChange> anim_basepaths;
  Vector<bNode *> nodes_delayed_free;
  const bNodeTree *ngroup = reinterpret_cast<const bNodeTree *>(gnode->id);

  /* `wgroup` is a temporary copy of the #NodeTree we're merging in
   * - All of wgroup's nodes are copied across to their new home.
   * - `ngroup` (i.e. the source NodeTree) is left unscathed.
   * - Temp copy. do change ID user-count for the copies.
   */
  bNodeTree *wgroup = bke::node_tree_copy_tree(bmain, *ngroup);

  /* Add the nodes into the `ntree`. */
  Vector<bNode *> new_nodes;
  Map<int32_t, int32_t> node_identifier_map;
  for (bNode &node : wgroup->nodes.items_mutable()) {
    new_nodes.append(&node);
    /* Remove interface nodes.
     * This also removes remaining links to and from interface nodes.
     */
    if (node.is_group_input() || node.is_group_output()) {
      /* We must delay removal since sockets will reference this node. see: #52092 */
      nodes_delayed_free.append(&node);
    }

    /* Keep track of this node's RNA "base" path (the part of the path identifying the node). */
    const std::string old_basepath = node_basepath(*wgroup, node);

    /* migrate node */
    BLI_remlink(&wgroup->nodes, &node);
    BLI_addtail(&ntree->nodes, &node);
    const int32_t old_identifier = node.identifier;
    bke::node_unique_id(*ntree, node);
    bke::node_unique_name(*ntree, node);
    node_identifier_map.add(old_identifier, node.identifier);

    BKE_ntree_update_tag_node_new(ntree, &node);

    const std::string new_basepath = node_basepath(*ntree, node);
    anim_basepaths.append({old_basepath, new_basepath});

    node.location[0] += gnode->location[0];
    node.location[1] += gnode->location[1];

    node.flag |= NODE_SELECT;
  }
  wgroup->runtime->nodes_by_id.clear();

  bNodeLink *glinks_first = static_cast<bNodeLink *>(ntree->links.last);

  /* Add internal links to the ntree */
  for (bNodeLink &link : wgroup->links.items_mutable()) {
    BLI_remlink(&wgroup->links, &link);
    BLI_addtail(&ntree->links, &link);
    BKE_ntree_update_tag_link_added(ntree, &link);
  }

  bNodeLink *glinks_last = static_cast<bNodeLink *>(ntree->links.last);

  BKE_animdata_copy_by_basepath(*bmain, wgroup->id, ntree->id, anim_basepaths);

  remap_pairing(*ntree, new_nodes, node_identifier_map);

  /* free the group tree (takes care of user count) */
  BKE_id_free(bmain, wgroup);

  /* restore external links to and from the gnode */

  /* input links */
  if (glinks_first != nullptr) {
    for (bNodeLink *link = glinks_first->next; link != glinks_last->next; link = link->next) {
      if (link->fromnode->is_group_input()) {
        const char *identifier = link->fromsock->identifier;
        int num_external_links = 0;

        /* find external links to this input */
        for (bNodeLink *tlink = static_cast<bNodeLink *>(ntree->links.first);
             tlink != glinks_first->next;
             tlink = tlink->next)
        {
          if (tlink->tonode == gnode && STREQ(tlink->tosock->identifier, identifier)) {
            bke::node_add_link(
                *ntree, *tlink->fromnode, *tlink->fromsock, *link->tonode, *link->tosock);
            num_external_links++;
          }
        }

        /* if group output is not externally linked,
         * convert the constant input value to ensure somewhat consistent behavior */
        if (num_external_links == 0) {
          /* TODO */
#if 0
          bNodeSocket *sock = node_group_find_input_socket(gnode, identifier);
          BLI_assert(sock);

          nodeSocketCopy(
              ntree, link->tosock->new_sock, link->tonode->new_node, ntree, sock, gnode);
#endif
        }
      }
    }

    /* Also iterate over new links to cover passthrough links. */
    glinks_last = static_cast<bNodeLink *>(ntree->links.last);

    /* output links */
    for (bNodeLink *link = static_cast<bNodeLink *>(ntree->links.first);
         link != glinks_first->next;
         link = link->next)
    {
      if (link->fromnode == gnode) {
        const char *identifier = link->fromsock->identifier;
        int num_internal_links = 0;

        /* find internal links to this output */
        for (bNodeLink *tlink = glinks_first->next; tlink != glinks_last->next;
             tlink = tlink->next)
        {
          /* only use active output node */
          if (tlink->tonode->is_group_output() && (tlink->tonode->flag & NODE_DO_OUTPUT)) {
            if (STREQ(tlink->tosock->identifier, identifier)) {
              bke::node_add_link(
                  *ntree, *tlink->fromnode, *tlink->fromsock, *link->tonode, *link->tosock);
              num_internal_links++;
            }
          }
        }

        /* if group output is not internally linked,
         * convert the constant output value to ensure somewhat consistent behavior */
        if (num_internal_links == 0) {
          /* TODO */
#if 0
          bNodeSocket *sock = node_group_find_output_socket(gnode, identifier);
          BLI_assert(sock);

          nodeSocketCopy(ntree, link->tosock, link->tonode, ntree, sock, gnode);
#endif
        }
      }
    }
  }

  for (bNode *node : nodes_delayed_free) {
    bke::node_remove_node(bmain, *ntree, *node, false);
  }

  update_nested_node_refs_after_ungroup(*ntree, *ngroup, *gnode, node_identifier_map);

  /* delete the group instance and dereference group tree */
  bke::node_remove_node(bmain, *ntree, *gnode, true);
}

static wmOperatorStatus node_group_ungroup_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  const StringRef node_idname = node_group_idname(C);

  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);

  Vector<bNode *> nodes_to_ungroup;
  for (bNode *node : snode->edittree->all_nodes()) {
    if (node->flag & NODE_SELECT) {
      if (node->idname == node_idname) {
        if (node->id != nullptr) {
          nodes_to_ungroup.append(node);
        }
      }
    }
  }
  if (nodes_to_ungroup.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  for (bNode *node : nodes_to_ungroup) {
    node_group_ungroup(bmain, snode->edittree, node);
  }
  BKE_main_ensure_invariants(*CTX_data_main(C));
  return OPERATOR_FINISHED;
}

void NODE_OT_group_ungroup(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Ungroup";
  ot->description = "Ungroup selected nodes";
  ot->idname = "NODE_OT_group_ungroup";

  /* API callbacks. */
  ot->exec = node_group_ungroup_exec;
  ot->poll = node_group_operator_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Separate Operator
 * \{ */

/**
 * \return True if successful.
 */
static bool node_group_separate_selected(
    Main &bmain, bNodeTree &ntree, bNodeTree &ngroup, const float2 &offset, const bool make_copy)
{
  node_deselect_all(ntree);

  Vector<AnimationBasePathChange> anim_basepaths;
  Map<bNode *, bNode *> node_map;
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  Map<int32_t, int32_t> node_identifier_map;

  /* Add selected nodes into the ntree, ignoring interface nodes. */
  VectorSet<bNode *> nodes_to_move = get_selected_nodes(ngroup);
  nodes_to_move.remove_if(
      [](const bNode *node) { return node->is_group_input() || node->is_group_output(); });

  for (bNode *node : nodes_to_move) {
    const std::string old_basepath = node_basepath(ngroup, *node);
    bNode *newnode;
    if (make_copy) {
      newnode = bke::node_copy_with_mapping(
          &ntree, *node, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
      node_identifier_map.add(node->identifier, newnode->identifier);
    }
    else {
      newnode = node;
      BLI_remlink(&ngroup.nodes, newnode);
      BLI_addtail(&ntree.nodes, newnode);
      const int32_t old_identifier = node->identifier;
      bke::node_unique_id(ntree, *newnode);
      bke::node_unique_name(ntree, *newnode);
      node_identifier_map.add(old_identifier, newnode->identifier);
    }
    node_map.add_new(node, newnode);

    /* Keep track of this node's RNA "base" path (the part of the path identifying the node). */
    const std::string new_basepath = node_basepath(ngroup, *newnode);
    anim_basepaths.append({old_basepath, new_basepath});

    newnode->location[0] += offset.x;
    newnode->location[1] += offset.y;
  }
  for (bNode *newnode : node_map.values()) {
    /* Ensure valid parent pointers. Detach if parent stays inside the group. */
    if (newnode->parent) {
      if (newnode->parent->flag & NODE_SELECT) {
        newnode->parent = node_map.lookup(newnode->parent);
      }
      else {
        bke::node_detach_node(ngroup, *newnode);
      }
    }
  }
  if (!make_copy) {
    bke::node_rebuild_id_vector(ngroup);
  }

  /* add internal links to the ntree */
  for (bNodeLink &link : ngroup.links.items_mutable()) {
    const bool fromselect = (link.fromnode && nodes_to_move.contains(link.fromnode));
    const bool toselect = (link.tonode && nodes_to_move.contains(link.tonode));

    if (make_copy) {
      /* make a copy of internal links */
      if (fromselect && toselect) {
        bke::node_add_link(ntree,
                           *node_map.lookup(link.fromnode),
                           *socket_map.lookup(link.fromsock),
                           *node_map.lookup(link.tonode),
                           *socket_map.lookup(link.tosock));
      }
    }
    else {
      /* move valid links over, delete broken links */
      if (fromselect && toselect) {
        BLI_remlink(&ngroup.links, &link);
        BLI_addtail(&ntree.links, &link);
      }
      else if (fromselect || toselect) {
        bke::node_remove_link(&ngroup, link);
      }
    }
  }

  remap_pairing(ntree, nodes_to_move, node_identifier_map);

  for (bNode *node : node_map.values()) {
    bke::node_declaration_ensure(ntree, *node);
  }

  /* and copy across the animation,
   * note that the animation data's action can be nullptr here */
  if (make_copy) {
    BKE_animdata_copy_by_basepath(bmain, ngroup.id, ntree.id, anim_basepaths);
  }
  else {
    BKE_animdata_move_by_basepath(bmain, ngroup.id, ntree.id, anim_basepaths);
  }

  BKE_ntree_update_tag_all(&ntree);
  if (!make_copy) {
    BKE_ntree_update_tag_all(&ngroup);
  }

  return true;
}

enum eNodeGroupSeparateType {
  NODE_GS_COPY,
  NODE_GS_MOVE,
};

/* Operator Property */
static const EnumPropertyItem node_group_separate_types[] = {
    {NODE_GS_COPY, "COPY", 0, "Copy", "Copy to parent node tree, keep group intact"},
    {NODE_GS_MOVE, "MOVE", 0, "Move", "Move to parent node tree, remove from group"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus node_group_separate_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  int type = RNA_enum_get(op->ptr, "type");

  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);

  /* are we inside of a group? */
  bNodeTree *ngroup = snode->edittree;
  bNodeTree *nparent = ED_node_tree_get(snode, 1);
  if (!nparent) {
    BKE_report(op->reports, RPT_WARNING, "Not inside node group");
    return OPERATOR_CANCELLED;
  }
  /* get node tree offset */
  const float2 offset = space_node_group_offset(*snode);

  switch (type) {
    case NODE_GS_COPY:
      if (!node_group_separate_selected(*bmain, *nparent, *ngroup, offset, true)) {
        BKE_report(op->reports, RPT_WARNING, "Cannot separate nodes");
        return OPERATOR_CANCELLED;
      }
      break;
    case NODE_GS_MOVE:
      if (!node_group_separate_selected(*bmain, *nparent, *ngroup, offset, false)) {
        BKE_report(op->reports, RPT_WARNING, "Cannot separate nodes");
        return OPERATOR_CANCELLED;
      }
      break;
  }

  /* switch to parent tree */
  ED_node_tree_pop(region, snode);

  BKE_main_ensure_invariants(*CTX_data_main(C));

  return OPERATOR_FINISHED;
}

static wmOperatorStatus node_group_separate_invoke(bContext *C,
                                                   wmOperator * /*op*/,
                                                   const wmEvent * /*event*/)
{
  ui::PopupMenu *pup = ui::popup_menu_begin(
      C, CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Separate"), ICON_NONE);
  ui::Layout *layout = popup_menu_layout(pup);

  layout->operator_context_set(wm::OpCallContext::ExecDefault);
  PointerRNA op_ptr = layout->op("NODE_OT_group_separate", IFACE_("Copy"), ICON_NONE);
  RNA_enum_set(&op_ptr, "type", NODE_GS_COPY);
  op_ptr = layout->op("NODE_OT_group_separate", IFACE_("Move"), ICON_NONE);
  RNA_enum_set(&op_ptr, "type", NODE_GS_MOVE);

  popup_menu_end(C, pup);

  return OPERATOR_INTERFACE;
}

void NODE_OT_group_separate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Separate";
  ot->description = "Separate selected nodes from the node group";
  ot->idname = "NODE_OT_group_separate";

  /* API callbacks. */
  ot->invoke = node_group_separate_invoke;
  ot->exec = node_group_separate_exec;
  ot->poll = node_group_operator_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "type", node_group_separate_types, NODE_GS_COPY, "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Make Group Operator
 * \{ */

/**
 * Set of nodes that are copied from other nodes and can be mapped to the original nodes.
 */
class NodeSetCopy {
 public:
  struct GroupInputOutputNodes {
    bNode *input_node;
    bNode *output_node;
  };

 private:
  bNodeTree &tree_;
  Map<const bNode *, bNode *> node_map_;
  Map<const bNodeSocket *, bNodeSocket *> socket_map_;
  Map<int32_t, int32_t> node_identifier_map_;

 public:
  const Map<int32_t, int32_t> &node_identifier_map() const
  {
    return node_identifier_map_;
  }

  static NodeSetCopy from_nodes(Main &bmain,
                                const bNodeTree &src_tree,
                                const Span<const bNode *> src_nodes,
                                bNodeTree &dst_tree)
  {
    node_deselect_all(dst_tree);

    NodeSetCopy result(dst_tree);
    Vector<AnimationBasePathChange> anim_basepaths;
    for (const bNode *src_node : src_nodes) {
      bNode *dst_node = bke::node_copy_with_mapping(&dst_tree,
                                                    *src_node,
                                                    LIB_ID_COPY_DEFAULT,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    result.socket_map_);

      result.node_map_.add(src_node, dst_node);
      result.node_identifier_map_.add(src_node->identifier, dst_node->identifier);

      anim_basepaths.append(
          {node_basepath(src_tree, *src_node), node_basepath(dst_tree, *dst_node)});
    }

    /* Recreate parent/child relationship of nodes. */
    for (const auto &item : result.node_map_.items()) {
      const bNode *node = item.key;
      bNode *new_node = item.value;
      if (node->parent) {
        if (bNode *new_parent = result.node_map_.lookup_default(node->parent, nullptr)) {
          bke::node_attach_node(dst_tree, *new_node, *new_parent);
        }
        else {
          bke::node_detach_node(dst_tree, *new_node);
        }
      }
    }

    /* Recreate internal links. */
    const Vector<const bNodeLink *> internal_links = util::find_internal_links(src_tree,
                                                                               src_nodes);
    for (const bNodeLink *src_link : internal_links) {
      bke::node_add_link(dst_tree,
                         *result.node_map_.lookup(src_link->fromnode),
                         *result.socket_map_.lookup(src_link->fromsock),
                         *result.node_map_.lookup(src_link->tonode),
                         *result.socket_map_.lookup(src_link->tosock));
    }

    /* Recreate zone pairing between new nodes. */
    const Vector<bNode *> new_nodes(result.node_map_.values().begin(),
                                    result.node_map_.values().end());
    remap_pairing(dst_tree, new_nodes, result.node_identifier_map_);

    /* Copy animation data of source nodes. */
    BKE_animdata_copy_by_basepath(bmain, src_tree.id, dst_tree.id, anim_basepaths);

    /* Move nodes in the group to the center */
    if (const std::optional<Bounds<float2>> bounds = node_location_bounds(src_nodes)) {
      const float2 center = bounds->center();
      for (bNode *node : new_nodes) {
        node->location[0] -= center[0];
        node->location[1] -= center[1];
      }
    }

    return result;
  }

  GroupInputOutputNodes expose_sockets_to_interface(const bContext &C,
                                                    const NodeSetInterface &node_set_io) const
  {
    Main &bmain = *CTX_data_main(&C);
    tree_.ensure_topology_cache();

    GroupInputOutputNodes io_nodes;
    io_nodes.output_node = tree_.group_output_node();
    if (!io_nodes.output_node) {
      io_nodes.output_node = bke::node_add_static_node(&C, tree_, NODE_GROUP_OUTPUT);
    }
    io_nodes.input_node = bke::node_add_static_node(&C, tree_, NODE_GROUP_INPUT);

    /* This makes sure that all nodes have the correct sockets so that we can link. */
    BKE_main_ensure_invariants(bmain, tree_.id);

    for (const auto &item : node_set_io.socket_data().items()) {
      for (const NodeSocketRef &origin : item.value.internal_sockets) {
        bNode *new_node = node_map_.lookup(&origin.node);
        bNodeSocket *new_socket = socket_map_.lookup(&origin.socket);
        if (origin.socket.is_input()) {
          bNodeSocket *group_input_socket = node_group_input_find_socket(io_nodes.input_node,
                                                                         item.key->identifier);
          BLI_assert(group_input_socket);
          bke::node_add_link(
              tree_, *io_nodes.input_node, *group_input_socket, *new_node, *new_socket);
        }
        else {
          bNodeSocket *group_output_socket = node_group_output_find_socket(io_nodes.output_node,
                                                                           item.key->identifier);
          BLI_assert(group_output_socket);
          bke::node_add_link(
              tree_, *new_node, *new_socket, *io_nodes.output_node, *group_output_socket);
        }
      }
    }

    /* Make sure group input/output node sockets match the tree interface. */
    nodes::update_node_declaration_and_sockets(tree_, *io_nodes.input_node);
    nodes::update_node_declaration_and_sockets(tree_, *io_nodes.output_node);

    /* Move group input/output nodes to the edges of the bounding box. */
    if (const std::optional<Bounds<float2>> bounds = node_bounds(node_map_.values())) {
      io_nodes.input_node->location[0] = bounds->min[0] - 200.0f;
      io_nodes.input_node->location[1] = bounds->center()[1];
      io_nodes.output_node->location[0] = bounds->max[0] + 50.0f;
      io_nodes.output_node->location[1] = bounds->center()[1];
    }

    return io_nodes;
  }

 private:
  NodeSetCopy(bNodeTree &tree) : tree_(tree) {}
};

static VectorSet<bNode *> get_nodes_to_group(bNodeTree &node_tree, bNode *group_node)
{
  VectorSet<bNode *> nodes_to_group = get_selected_nodes(node_tree);
  nodes_to_group.remove_if(
      [](bNode *node) { return node->is_group_input() || node->is_group_output(); });
  nodes_to_group.remove(group_node);
  return nodes_to_group;
}

static bool node_group_make_test_selected(bNodeTree &ntree,
                                          const VectorSet<bNode *> &nodes_to_group,
                                          const StringRef ntree_idname,
                                          ReportList &reports)
{
  if (nodes_to_group.is_empty()) {
    return false;
  }
  /* make a local pseudo node tree to pass to the node poll functions */
  bNodeTree *ngroup = bke::node_tree_add_tree(nullptr, "Pseudo Node Group", ntree_idname);
  BLI_SCOPED_DEFER([&]() {
    bke::node_tree_free_tree(*ngroup);
    MEM_freeN(ngroup);
  });

  /* check poll functions for selected nodes */
  for (bNode *node : nodes_to_group) {
    const char *disabled_hint = nullptr;
    if (node->typeinfo->poll_instance &&
        !node->typeinfo->poll_instance(node, ngroup, &disabled_hint))
    {
      if (disabled_hint) {
        BKE_reportf(&reports,
                    RPT_WARNING,
                    "Cannot add node '%s' in a group:\n  %s",
                    node->name,
                    disabled_hint);
      }
      else {
        BKE_reportf(&reports, RPT_WARNING, "Cannot add node '%s' in a group", node->name);
      }
      return false;
    }
  }

  /* check if all connections are OK, no unselected node has both
   * inputs and outputs to a selection */
  ntree.ensure_topology_cache();
  for (bNode *node : ntree.all_nodes()) {
    if (nodes_to_group.contains(node)) {
      continue;
    }
    auto sockets_connected_to_group = [&](const Span<bNodeSocket *> sockets) {
      for (const bNodeSocket *socket : sockets) {
        for (const bNodeSocket *other_socket : socket->directly_linked_sockets()) {
          if (nodes_to_group.contains(const_cast<bNode *>(&other_socket->owner_node()))) {
            return true;
          }
        }
      }
      return false;
    };
    if (sockets_connected_to_group(node->input_sockets()) &&
        sockets_connected_to_group(node->output_sockets()))
    {
      return false;
    }
  }
  /* Check if zone pairs are fully selected.
   * Zone input or output nodes can only be grouped together with the paired node. */
  for (const bke::bNodeZoneType *zone_type : bke::all_zone_types()) {
    for (bNode *input_node : ntree.nodes_by_type(zone_type->input_idname)) {
      if (bNode *output_node = zone_type->get_corresponding_output(ntree, *input_node)) {
        const bool input_selected = nodes_to_group.contains(input_node);
        const bool output_selected = nodes_to_group.contains(output_node);
        if (input_selected && !output_selected) {
          BKE_reportf(&reports,
                      RPT_WARNING,
                      "Cannot add zone input node '%s' to a group without its paired output '%s'",
                      input_node->name,
                      output_node->name);
          return false;
        }
        if (output_selected && !input_selected) {
          BKE_reportf(&reports,
                      RPT_WARNING,
                      "Cannot add zone output node '%s' to a group without its paired input '%s'",
                      output_node->name,
                      input_node->name);
          return false;
        }
      }
    }
  }

  return true;
}

static void update_nested_node_refs_after_moving_nodes_into_group(
    bNodeTree &ntree,
    bNodeTree &group,
    bNode &gnode,
    const Map<int32_t, int32_t> &node_identifier_map)
{
  /* Update nested node references in the parent and child node tree. */
  RandomNumberGenerator rng = RandomNumberGenerator::from_random_seed();
  Vector<bNestedNodeRef> new_nested_node_refs;
  /* Keep all nested node references that were in the group before. */
  for (const bNestedNodeRef &ref : group.nested_node_refs_span()) {
    new_nested_node_refs.append(ref);
  }
  Set<int32_t> used_nested_node_ref_ids;
  for (const bNestedNodeRef &ref : group.nested_node_refs_span()) {
    used_nested_node_ref_ids.add(ref.id);
  }
  for (bNestedNodeRef &ref : ntree.nested_node_refs_span()) {
    const int32_t new_node_id = node_identifier_map.lookup_default(ref.path.node_id, -1);
    if (new_node_id == -1) {
      /* The node was not moved between node groups. */
      continue;
    }
    bNestedNodeRef new_ref = ref;
    new_ref.path.node_id = new_node_id;
    /* Find new unique identifier for the nested node ref. */
    while (true) {
      const int32_t new_id = rng.get_int32(INT32_MAX);
      if (used_nested_node_ref_ids.add(new_id)) {
        new_ref.id = new_id;
        break;
      }
    }
    new_nested_node_refs.append(new_ref);
    /* Updated the nested node ref in the parent so that it points to the same node that is now
     * inside of a nested group. */
    ref.path.node_id = gnode.identifier;
    ref.path.id_in_node = new_ref.id;
  }
  MEM_SAFE_FREE(group.nested_node_refs);
  group.nested_node_refs = MEM_new_array_for_free<bNestedNodeRef>(new_nested_node_refs.size(),
                                                                  __func__);
  uninitialized_copy_n(
      new_nested_node_refs.data(), new_nested_node_refs.size(), group.nested_node_refs);
  group.nested_node_refs_num = new_nested_node_refs.size();
}

static void node_group_make_insert_selected(const bContext &C,
                                            bNodeTree &ntree,
                                            bNode *gnode,
                                            const Span<bNode *> nodes)
{
  Main &bmain = *CTX_data_main(&C);
  bNodeTree &group = *reinterpret_cast<bNodeTree *>(gnode->id);

  /* If only one node is selected expose all its sockets regardless of links. */
  const bool expose_visible = nodes.size() == 1;
  const NodeSetInterface node_set_io = NodeSetInterface::from_nodes(
      ntree, nodes, group, expose_visible);
  /* Copy nodes into the group. */
  const NodeSetCopy node_set_copy = NodeSetCopy::from_nodes(bmain, ntree, nodes, group);
  /* Connect exposed sockets to group input/output nodes. */
  node_set_copy.expose_sockets_to_interface(C, node_set_io);

  update_nested_node_refs_after_moving_nodes_into_group(
      ntree, group, *gnode, node_set_copy.node_identifier_map());
  BKE_main_ensure_invariants(bmain, Span<ID *>{&group.id});

  /* Connect the group node to external sockets. */
  node_set_io.connect_group_node(*gnode);

  /* Remove original nodes from the tree, everything has been copied to the group. */
  for (bNode *node : nodes) {
    bke::node_remove_node(&bmain, ntree, *node, true);
  }

  BKE_main_ensure_invariants(bmain);
}

static bNode *node_group_make_from_nodes(const bContext &C,
                                         bNodeTree &ntree,
                                         const Span<bNode *> nodes_to_group,
                                         const StringRef ntype,
                                         const StringRef ntreetype)
{
  Main *bmain = CTX_data_main(&C);

  /* New node-tree. */
  bNodeTree *ngroup = bke::node_tree_add_tree(bmain, "NodeGroup", ntreetype);
  BKE_id_move_to_same_lib(*bmain, ngroup->id, ntree.id);

  /* make group node */
  bNode *gnode = bke::node_add_node(&C, ntree, ntype);
  gnode->id = id_cast<ID *>(ngroup);

  if (const std::optional<Bounds<float2>> bounds = node_location_bounds(nodes_to_group)) {
    gnode->location[0] = bounds->center()[0];
    gnode->location[1] = bounds->center()[1];
  }

  node_group_make_insert_selected(C, ntree, gnode, nodes_to_group);

  return gnode;
}

// struct WrapperNodeGroupMapping {
//   int num_inputs = 0;
//   int num_outputs = 0;
//   Map<const bNodeSocket *, int> new_index_by_src_socket;
//   Map<int, int> new_by_old_panel_identifier;
//   Vector<int> exposed_input_indices;
//   Vector<int> exposed_output_indices;
//   int inner_node_identifier;

//   bNodeSocket *get_new_input(const bNodeSocket *old_socket, bNode &new_node) const
//   {
//     if (const std::optional<int> index = new_index_by_src_socket.lookup_try(old_socket)) {
//       return &new_node.input_socket(*index);
//     }
//     return nullptr;
//   }

//   bNodeSocket *get_new_output(const bNodeSocket *old_socket, bNode &new_node) const
//   {
//     if (const std::optional<int> index = new_index_by_src_socket.lookup_try(old_socket)) {
//       return &new_node.output_socket(*index);
//     }
//     return nullptr;
//   }
// };

// static void add_node_group_interface_from_declaration_recursive(
//     bNodeTree &group,
//     const bNode &src_node,
//     const nodes::ItemDeclaration &item_decl,
//     bNodeTreeInterfacePanel *parent,
//     WrapperNodeGroupMapping &r_mapping)
// {
//   if (const nodes::SocketDeclaration *socket_decl = dynamic_cast<const
//   nodes::SocketDeclaration
//   *>(
//           &item_decl))
//   {
//     const bNodeSocket &socket = src_node.socket_by_decl(*socket_decl);
//     if (!socket.is_available()) {
//       return;
//     }
//     bNodeTreeInterfaceSocket *io_socket = bke::node_interface::add_interface_socket_from_node(
//         group, src_node, socket);
//     if (!io_socket) {
//       return;
//     }
//     group.tree_interface.move_item_to_parent(io_socket->item, parent, INT32_MAX);
//     if (socket.is_input()) {
//       r_mapping.new_index_by_src_socket.add_new(&socket, r_mapping.num_inputs++);
//       r_mapping.exposed_input_indices.append(socket.index());
//     }
//     else {
//       r_mapping.new_index_by_src_socket.add_new(&socket, r_mapping.num_outputs++);
//       r_mapping.exposed_output_indices.append(socket.index());
//     }
//   }
//   else if (const nodes::PanelDeclaration *panel_decl =
//                dynamic_cast<const nodes::PanelDeclaration *>(&item_decl))
//   {
//     NodeTreeInterfacePanelFlag flag{};
//     if (panel_decl->default_collapsed) {
//       flag |= NODE_INTERFACE_PANEL_DEFAULT_CLOSED;
//     }
//     bNodeTreeInterfacePanel *io_panel = group.tree_interface.add_panel(
//         panel_decl->name, panel_decl->description, flag, parent);
//     r_mapping.new_by_old_panel_identifier.add_new(panel_decl->identifier,
//     io_panel->identifier); for (const nodes::ItemDeclaration *child_item_decl :
//     panel_decl->items) {
//       add_node_group_interface_from_declaration_recursive(
//           group, src_node, *child_item_decl, io_panel, r_mapping);
//     }
//   }
// }

static std::tuple<bNodeTree *, const NodeSetInterface, const NodeSetCopy> node_group_make_wrapper(
    const bContext &C, const bNodeTree &src_tree, const bNode &src_node)
{
  Main &bmain = *CTX_data_main(&C);

  bNodeTree *dst_group = bke::node_tree_add_tree(
      &bmain, bke::node_label(src_tree, src_node), src_tree.idname);
  dst_group->color_tag = int(bke::node_color_tag(src_node));

  const NodeSetInterface node_set_io = NodeSetInterface::from_node_declaration(src_node,
                                                                               *dst_group);
  // const nodes::NodeDeclaration &node_decl = *src_node.declaration();
  // for (const nodes::ItemDeclaration *item_decl : node_decl.root_items) {
  //   add_node_group_interface_from_declaration_recursive(
  //       *dst_group, src_node, *item_decl, nullptr, r_mapping);
  // }

  const NodeSetCopy node_set_copy = NodeSetCopy::from_nodes(
      bmain, src_tree, {&src_node}, *dst_group);
  node_set_copy.expose_sockets_to_interface(C, node_set_io);

  // /* Add the node that make up the wrapper node group. */
  // bNode &input_node = *bke::node_add_static_node(&C, *dst_group, NODE_GROUP_INPUT);
  // bNode &output_node = *bke::node_add_static_node(&C, *dst_group, NODE_GROUP_OUTPUT);

  // Map<const bNodeSocket *, bNodeSocket *> inner_node_socket_mapping;
  // bNode &inner_node = *bke::node_copy_with_mapping(
  //     dst_group, src_node, 0, std::nullopt, std::nullopt, inner_node_socket_mapping);
  // r_mapping.inner_node_identifier = inner_node.identifier;

  // /* Position nodes. */
  // input_node.location[0] = -300 - input_node.width;
  // output_node.location[0] = 300;
  // inner_node.location[0] = -src_node.width / 2;
  // inner_node.location[1] = 0;
  // inner_node.width = src_node.width;
  // inner_node.parent = nullptr;

  // /* This makes sure that all nodes have the correct sockets so that we can link. */
  // BKE_main_ensure_invariants(bmain, dst_group->id);

  // /* Expand all panels in wrapper node group. */
  // for (bNodePanelState &panel_state : inner_node.panel_states()) {
  //   panel_state.flag &= ~NODE_PANEL_COLLAPSED;
  // }
  // /* Make all sockets visible in wrapper node group. */
  // for (bNodeSocket *socket : inner_node.input_sockets()) {
  //   socket->flag &= ~SOCK_HIDDEN;
  // }
  // for (bNodeSocket *socket : inner_node.output_sockets()) {
  //   socket->flag &= ~SOCK_HIDDEN;
  // }

  // const Array<bNodeSocket *> group_inputs = input_node.output_sockets().drop_back(1);
  // const Array<bNodeSocket *> group_outputs = output_node.input_sockets().drop_back(1);
  // const Array<bNodeSocket *> inner_inputs = inner_node.input_sockets();
  // const Array<bNodeSocket *> inner_outputs = inner_node.output_sockets();
  // BLI_assert(group_inputs.size() == r_mapping.exposed_input_indices.size());
  // BLI_assert(group_outputs.size() == r_mapping.exposed_output_indices.size());

  // /* Add links. */
  // for (const int i : group_inputs.index_range()) {
  //   bke::node_add_link(*dst_group,
  //                      input_node,
  //                      *group_inputs[i],
  //                      inner_node,
  //                      *inner_inputs[r_mapping.exposed_input_indices[i]]);
  // }
  // for (const int i : group_outputs.index_range()) {
  //   bke::node_add_link(*dst_group,
  //                      inner_node,
  //                      *inner_outputs[r_mapping.exposed_output_indices[i]],
  //                      output_node,
  //                      *group_outputs[i]);
  // }

  // const std::string old_basepath = node_basepath(src_tree, src_node);
  // const std::string new_basepath = node_basepath(*dst_group, inner_node);
  // BKE_animdata_copy_by_basepath(bmain, src_tree.id, dst_group->id, {{old_basepath,
  // new_basepath}});

  BKE_main_ensure_invariants(bmain, dst_group->id);
  return {dst_group, std::move(node_set_io), std::move(node_set_copy)};
}

static bNode *node_group_make_from_node_declaration(bContext &C,
                                                    bNodeTree &ntree,
                                                    bNode &src_node,
                                                    const StringRef node_idname)
{
  Main &bmain = *CTX_data_main(&C);

  // WrapperNodeGroupMapping mapping;
  auto [wrapper_group, node_set_io, node_set_copy] = node_group_make_wrapper(C, ntree, src_node);

  /* Create a group node. */
  bNode *gnode = bke::node_add_node(&C, ntree, node_idname);
  STRNCPY_UTF8(gnode->name, BKE_id_name(wrapper_group->id));
  bke::node_unique_name(ntree, *gnode);

  /* Assign the newly created wrapper group to the new group node. */
  gnode->id = &wrapper_group->id;

  /* Position node exactly where the old node was. */
  gnode->parent = src_node.parent;
  gnode->width = std::max<float>(src_node.width, GROUP_NODE_MIN_WIDTH);
  copy_v2_v2(gnode->location, src_node.location);

  BKE_main_ensure_invariants(bmain);
  ntree.ensure_topology_cache();

  node_set_io.connect_group_node(*gnode);

  // /* Keep old socket visibility. */
  // for (const bNodeSocket *src_socket : src_node.input_sockets())
  // {
  //   if (bNodeSocket *new_socket = mapping.get_new_input(src_socket, *gnode)) {
  //     new_socket->flag |= src_socket->flag & (SOCK_HIDDEN | SOCK_COLLAPSED);
  //   }
  // }
  // for (const bNodeSocket *src_socket : src_node.output_sockets()) {
  //   if (bNodeSocket *new_socket = mapping.get_new_output(src_socket, *gnode)) {
  //     new_socket->flag |= src_socket->flag & (SOCK_HIDDEN | SOCK_COLLAPSED);
  //   }
  // }

  // /* Keep old panel collapse status. */
  // const Span<bNodePanelState> src_panel_states = src_node.panel_states();
  // MutableSpan<bNodePanelState> new_panel_states = gnode->panel_states();
  // for (const bNodePanelState &src_panel_state : src_panel_states) {
  //   if (const std::optional<int> new_identifier =
  //   mapping.new_by_old_panel_identifier.lookup_try(
  //           src_panel_state.identifier))
  //   {
  //     for (bNodePanelState &new_panel_state : new_panel_states) {
  //       if (new_panel_state.identifier == *new_identifier) {
  //         SET_FLAG_FROM_TEST(new_panel_state.flag,
  //                            src_panel_state.flag & NODE_PANEL_COLLAPSED,
  //                            NODE_PANEL_COLLAPSED);
  //       }
  //     }
  //   }
  // }

  // /* Relink links from old to new node. */
  // for (bNodeLink &link : ntree.links.items_mutable()) {
  //   if (link.tonode == &src_node) {
  //     if (bNodeSocket *new_to_socket = mapping.get_new_input(link.tosock, *gnode)) {
  //       link.tonode = gnode;
  //       link.tosock = new_to_socket;
  //       continue;
  //     }
  //     bke::node_remove_link(&ntree, link);
  //     continue;
  //   }
  //   if (link.fromnode == &src_node) {
  //     if (bNodeSocket *new_from_socket = mapping.get_new_output(link.fromsock, *gnode)) {
  //       link.fromnode = gnode;
  //       link.fromsock = new_from_socket;
  //       continue;
  //     }
  //     bke::node_remove_link(&ntree, link);
  //     continue;
  //   }
  // }

  // Map<int32_t, int32_t> node_identifier_map;
  // node_identifier_map.add_new(src_node.identifier, mapping.inner_node_identifier);

  /* Remove the old node because it has been replaced. Use the name of the removed node for the
   * new group node. This also keeps animation data working. */
  std::string old_node_name = src_node.name;
  bke::node_remove_node(&bmain, ntree, src_node, true, false);
  STRNCPY(gnode->name, old_node_name.c_str());

  /* Clear already created nested node refs to create new stable ones below. */
  MEM_SAFE_FREE(wrapper_group->nested_node_refs);
  wrapper_group->nested_node_refs_num = 0;
  update_nested_node_refs_after_moving_nodes_into_group(
      ntree, *wrapper_group, *gnode, node_set_copy.node_identifier_map());

  BKE_ntree_update_tag_node_property(&ntree, gnode);
  BKE_main_ensure_invariants(bmain);
  return gnode;
}

static wmOperatorStatus node_group_make_exec(bContext *C, wmOperator *op)
{
  ARegion &region = *CTX_wm_region(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;
  const StringRef ntree_idname = group_ntree_idname(C);
  const StringRef node_idname = node_group_idname(C);
  Main *bmain = CTX_data_main(C);

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  VectorSet<bNode *> nodes_to_group = get_nodes_to_group(ntree, nullptr);
  if (!node_group_make_test_selected(ntree, nodes_to_group, ntree_idname, *op->reports)) {
    return OPERATOR_CANCELLED;
  }

  bNode *gnode = nullptr;
  if (nodes_to_group.size() == 1 && nodes_to_group[0]->declaration()) {
    gnode = node_group_make_from_node_declaration(*C, ntree, *nodes_to_group[0], node_idname);
  }
  else {
    gnode = node_group_make_from_nodes(
        *C, ntree, std::move(nodes_to_group), node_idname, ntree_idname);
  }

  if (gnode) {
    bNodeTree *ngroup = id_cast<bNodeTree *>(gnode->id);

    bke::node_set_active(ntree, *gnode);
    if (ngroup) {
      ED_node_tree_push(&region, &snode, ngroup, gnode);
    }
  }

  WM_event_add_notifier(C, NC_NODE | NA_ADDED, nullptr);

  /* We broke relations in node tree, need to rebuild them in the graphs. */
  DEG_relations_tag_update(bmain);

  return OPERATOR_FINISHED;
}

void NODE_OT_group_make(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Group";
  ot->description = "Make group from selected nodes";
  ot->idname = "NODE_OT_group_make";

  /* API callbacks. */
  ot->exec = node_group_make_exec;
  ot->poll = node_group_operator_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Group Insert Operator
 * \{ */

static wmOperatorStatus node_group_insert_exec(bContext *C, wmOperator *op)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  ARegion *region = CTX_wm_region(C);
  bNodeTree *ntree = snode->edittree;
  const StringRef node_idname = node_group_idname(C);

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNode *gnode = node_group_get_active(C, node_idname);
  if (!gnode || !gnode->id) {
    return OPERATOR_CANCELLED;
  }

  bNodeTree *ngroup = reinterpret_cast<bNodeTree *>(gnode->id);
  VectorSet<bNode *> nodes_to_group = get_nodes_to_group(*ntree, gnode);

  /* Make sure that there won't be a node group containing itself afterwards. */
  for (const bNode *group : nodes_to_group) {
    if (!group->is_group() || group->id == nullptr) {
      continue;
    }
    if (bke::node_tree_contains_tree(*reinterpret_cast<bNodeTree *>(group->id), *ngroup)) {
      BKE_reportf(
          op->reports, RPT_WARNING, "Cannot insert group '%s' in '%s'", group->name, gnode->name);
      return OPERATOR_CANCELLED;
    }
  }

  if (!node_group_make_test_selected(*ntree, nodes_to_group, ngroup->idname, *op->reports)) {
    return OPERATOR_CANCELLED;
  }

  node_group_make_insert_selected(*C, *ntree, gnode, nodes_to_group);

  bke::node_set_active(*ntree, *gnode);
  ED_node_tree_push(region, snode, ngroup, gnode);

  return OPERATOR_FINISHED;
}

void NODE_OT_group_insert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Group Insert";
  ot->description = "Insert selected nodes into a node group";
  ot->idname = "NODE_OT_group_insert";

  /* API callbacks. */
  ot->exec = node_group_insert_exec;
  ot->poll = node_group_operator_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Default Group Width Operator
 * \{ */

static bool node_default_group_width_set_poll(bContext *C)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (!snode) {
    return false;
  }
  bNodeTree *ntree = snode->edittree;
  if (!ntree) {
    return false;
  }
  if (!ID_IS_EDITABLE(ntree)) {
    return false;
  }
  if (snode->nodetree == snode->edittree) {
    /* Top-level node group does not have enough context to set the node width. */
    CTX_wm_operator_poll_msg_set(C, "There is no parent group node in this context");
    return false;
  }
  return true;
}

static wmOperatorStatus node_default_group_width_set_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *ntree = snode->edittree;

  bNodeTreePath *last_path_item = static_cast<bNodeTreePath *>(snode->treepath.last);
  bNodeTreePath *parent_path_item = last_path_item->prev;
  if (!parent_path_item) {
    return OPERATOR_CANCELLED;
  }
  bNodeTree *parent_ntree = parent_path_item->nodetree;
  if (!parent_ntree) {
    return OPERATOR_CANCELLED;
  }
  parent_ntree->ensure_topology_cache();
  bNode *parent_node = bke::node_find_node_by_name(*parent_ntree, last_path_item->node_name);
  if (!parent_node) {
    return OPERATOR_CANCELLED;
  }
  ntree->default_group_node_width = parent_node->width;
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  return OPERATOR_CANCELLED;
}

void NODE_OT_default_group_width_set(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Default Group Node Width";
  ot->description = "Set the width based on the parent group node in the current context";
  ot->idname = "NODE_OT_default_group_width_set";

  /* API callbacks. */
  ot->exec = node_default_group_width_set_exec;
  ot->poll = node_default_group_width_set_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::space_node
