/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_bounds.hh"
#include "BLI_compute_context.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "BKE_compute_context_cache_fwd.hh"

#include "NOD_geometry_nodes_closure_location.hh"
#include "NOD_nested_node_id.hh"

#include "ED_node_c.hh"

#include "UI_interface_layout.hh"

namespace blender {

struct SpaceNode;
struct ARegion;
struct Main;
struct bContext;
struct bNodeSocket;
struct bNodeTree;
struct bNodeTreeInterfacePanel;
struct bNodeTreeInterfaceSocket;
struct Object;
struct rcti;
struct rctf;
struct NodesModifierData;

namespace bke {
class bNodeTreeZone;
}

namespace ui {
struct Layout;
}  // namespace ui

namespace nodes {
class ItemDeclaration;
}

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

namespace ed::space_node {

void tree_update(const bContext *C);

float grid_size_get();

/** Update the active node tree based on the context. */
void snode_set_context(const bContext &C);

VectorSet<bNode *> get_selected_nodes(bNodeTree &node_tree);

/**
 * \param is_new_node: If the node was just inserted, it is allowed to be inserted in a link, even
 * if it is linked already (after link-drag-search).
 */
void node_insert_on_link_flags_set(SpaceNode &snode,
                                   const ARegion &region,
                                   bool attach_enabled,
                                   bool is_new_node);

/**
 * Tag the editor to highlight the frame that currently transformed nodes will be attached to.
 */
void node_insert_on_frame_flag_set(bContext &C, SpaceNode &snode, const int2 &cursor);
void node_insert_on_frame_flag_clear(SpaceNode &snode);

/**
 * Assumes link with #NODE_LINK_INSERT_TARGET set.
 */
void node_insert_on_link_flags(Main &bmain, SpaceNode &snode, bool is_new_node);
void node_insert_on_link_flags_clear(bNodeTree &node_tree);

/**
 * Draw a single node socket at default size.
 */
void node_socket_draw(bNodeSocket *sock, const rcti *rect, const float color[4], float scale);
void node_draw_nodesocket(const rctf *rect,
                          const float color_inner[4],
                          const float color_outline[4],
                          float outline_thickness,
                          int shape,
                          float aspect);

void std_node_socket_colors_get(int socket_type, float *r_color);

/**
 * Find the nested node id of a currently visible node in the root tree.
 */
std::optional<nodes::FoundNestedNodeID> find_nested_node_id_in_root(const SpaceNode &snode,
                                                                    const bNode &node);
std::optional<nodes::FoundNestedNodeID> find_nested_node_id_in_root(
    const bNodeTree &root_tree, const ComputeContext *compute_context, const int node_id);

struct ObjectAndModifier {
  const Object *object;
  const NodesModifierData *nmd;
};
/**
 * Finds the context-modifier for the node editor.
 */
std::optional<ObjectAndModifier> get_modifier_for_node_editor(const SpaceNode &snode);

bool node_editor_is_for_geometry_nodes_modifier(const SpaceNode &snode,
                                                const Object &object,
                                                const NodesModifierData &nmd);

/**
 * Get the compute context for the active context that the user is currently looking at in that
 * node tree.
 */
[[nodiscard]] const ComputeContext *compute_context_for_edittree(
    const SpaceNode &snode, bke::ComputeContextCache &compute_context_cache);

/**
 * Get the active compute context for the given socket in the current edittree.
 */
[[nodiscard]] const ComputeContext *compute_context_for_edittree_socket(
    const SpaceNode &snode,
    bke::ComputeContextCache &compute_context_cache,
    const bNodeSocket &socket);

[[nodiscard]] const ComputeContext *compute_context_for_edittree_node(
    const SpaceNode &snode, bke::ComputeContextCache &compute_context_cache, const bNode &node);

/**
 * Creates a compute context for the given zone. It takes e.g. the current inspection index into
 * account.
 */
[[nodiscard]] const ComputeContext *compute_context_for_zone(
    const bke::bNodeTreeZone &zone,
    bke::ComputeContextCache &compute_context_cache,
    const ComputeContext *parent_compute_context);
[[nodiscard]] const ComputeContext *compute_context_for_zones(
    const Span<const bke::bNodeTreeZone *> zones,
    bke::ComputeContextCache &compute_context_cache,
    const ComputeContext *parent_compute_context);

void ui_template_node_asset_menu_items(ui::Layout &layout,
                                       const bContext &C,
                                       StringRef catalog_path,
                                       const ui::NodeAssetMenuOperatorType operator_type);

/** See #ed::space_node::SpaceNode_Runtime::node_can_sync_states. */
Map<int, bool> &node_can_sync_cache_get(SpaceNode &snode);

void node_tree_interface_draw(bContext &C, ui::Layout &layout, bNodeTree &tree);

const char *node_socket_get_label(const bNodeSocket *socket, const char *panel_label = nullptr);

const char *node_socket_get_description(const bNodeSocket *socket);

std::optional<Bounds<float2>> node_bounds(Span<const bNode *> nodes);
std::optional<Bounds<float2>> node_location_bounds(Span<const bNode *> nodes);

/* -------------------------------------------------------------------- */
/** \name Utilities for copying node sets
 * \{ */

/**
 * Controls the behavior of interface generator functions.
 */
struct NodeSetInterfaceParams {
  /* Hidden sockets are not added to the interface. */
  bool skip_hidden = false;
  /* Only sockets with external connections are added to the interface. */
  bool skip_unconnected = true;
  /* Register links of the group node as external links.
   * Otherwise interface sockets are externally disconnected. */
  bool add_external_links = true;
  /* Create a unique interface for every exposed input.
   * Otherwise inputs linked to the same socket use the same interface. */
  bool use_unique_input = true;
  /* Create a unique interface for every output connection.
   * Otherwise outputs with multiple connections create a single interface. */
  bool use_unique_output = false;
};

/**
 * Maps a subset of tree interface items to internal and external sockets.
 */
class NodeTreeInterfaceMapping {
 public:
  struct InterfaceSocketData {
    /* Sockets inside the group node tree. */
    VectorSet<NodeSocketRef> internal_sockets;
    /* External sockets to connect the interface. */
    VectorSet<MutableNodeSocketRef> external_sockets;
    /* New group node socket is hidden. */
    bool hidden = false;
    /* New group node socket is collapsed in tree view UI. */
    bool collapsed = false;
  };
  struct InterfacePanelData {
    /* New group node panel is collapsed. */
    bool collapsed = false;
  };

  Map<const bNodeTreeInterfaceSocket *, InterfaceSocketData> socket_data;
  Map<const bNodeTreeInterfacePanel *, InterfacePanelData> panel_data;
};

/**
 * Construct new interface sockets between internal and external nodes.
 * Sockets inside the \a src_nodes set are exposed if they have a link to an external node, or if
 * \a params.skip_unconnected is false.
 * Sockets outside the \a src_nodes set with links to internal sockets are connected to the new
 * interface sockets.
 */
NodeTreeInterfaceMapping build_node_set_interface(const NodeSetInterfaceParams &params,
                                                  const bNodeTree &src_tree,
                                                  const Span<bNode *> src_nodes,
                                                  bNodeTree &dst_tree);
/**
 * Construct new interface sockets based on the declaration of a single node.
 * This recreates the layout of the \a src_node exactly, including the panel structure.
 */
NodeTreeInterfaceMapping build_node_declaration_interface(const NodeSetInterfaceParams &params,
                                                          const bNode &src_node,
                                                          bNodeTree &dst_tree);
/**
 * Map the existing node group interface to internal nodes and external connections of the group
 * node. No new sockets are added to the interface.
 */
NodeTreeInterfaceMapping map_group_node_interface(const NodeSetInterfaceParams &params,
                                                  const bNode &group_node);

/**
 * Set of nodes that are copied from other nodes and can be mapped to the original nodes.
 */
class NodeSetCopy {
 private:
  bNodeTree &tree_;
  Map<const bNode *, bNode *> node_map_;
  Map<const bNodeSocket *, bNodeSocket *> socket_map_;
  Map<int32_t, int32_t> node_identifier_map_;

 public:
  bNodeTree &tree() const;
  const Map<const bNode *, bNode *> &node_map() const;
  const Map<const bNodeSocket *, bNodeSocket *> &socket_map() const;
  const Map<int32_t, int32_t> &node_identifier_map() const;

  static NodeSetCopy from_nodes(Main &bmain,
                                const bNodeTree &src_tree,
                                const Span<const bNode *> src_nodes,
                                bNodeTree &dst_tree);
  static NodeSetCopy from_predicate(Main &bmain,
                                    const bNodeTree &src_tree,
                                    FunctionRef<bool(const bNode &node)> node_predicate,
                                    bNodeTree &dst_tree);

 private:
  NodeSetCopy(bNodeTree &tree) : tree_(tree) {}
};

struct GroupInputOutputNodes {
  bNode *input_node;
  bNode *output_node;
};

/**
 * Connect copied node sockets to group node input/output nodes, recreating the interface mapping
 * of original nodes. The owner tree of the copied nodes must be the same as the interface tree.
 */
GroupInputOutputNodes connect_copied_nodes_to_interface(
    const bContext &C,
    const NodeSetCopy &copied_nodes,
    const NodeTreeInterfaceMapping &io_mapping);

/**
 * Connect copied node sockets to external nodes in the interface mapping.
 */
void connect_copied_nodes_to_external_sockets(const NodeSetCopy &copied_nodes,
                                              const NodeTreeInterfaceMapping &io_mapping);

/**
 * Connect the group node to external sockets in the interface mapping.
 * The group node must be in the same node tree as the mapped external sockets.
 */
void connect_group_node_to_external_sockets(bNode &group_node,
                                            const NodeTreeInterfaceMapping &io_mapping);

/**
 * Move nested node refs from nodes in \a ntree into the \a group_node tree.
 * Any node ref found in the \a node_identifier_map is recreated inside the group. The original
 * node refs in \a ntree are replaced by nested node refs pointing to the \a group_node.
 */
void update_nested_node_refs_after_moving_nodes_into_group(
    bNodeTree &tree, bNode &group_node, const Map<int32_t, int32_t> &node_identifier_map);

/**
 * Copy nested node refs from nodes in \a group_node into \a tree.
 * Any node ref found in the \a node_identifier_map is recreated inside \a tree, pointing to nested
 * node refs inside \a group_node.
 */
void update_nested_node_refs_after_ungroup(bNodeTree &tree,
                                           const bNode &group_node,
                                           const Map<int32_t, int32_t> &node_identifier_map);

/** \} */

}  // namespace ed::space_node

}  // namespace blender
