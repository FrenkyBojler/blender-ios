/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_stack.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_tree_interface_types.h"
#include "NOD_node_declaration.hh"

namespace blender::bke::node_structure_type_inferencing {

using nodes::StructureType;
namespace aal = nodes::anonymous_attribute_lifetime;

static nodes::StructureTypeInterface calc_node_interface(const bNode &node)
{
  const Span<const bNodeSocket *> input_sockets = node.input_sockets();
  const Span<const bNodeSocket *> output_sockets = node.output_sockets();

  nodes::StructureTypeInterface interface;
  interface.inputs.reinitialize(input_sockets.size());
  interface.outputs.reinitialize(output_sockets.size());

  if (node.is_undefined()) {
    interface.inputs.fill(StructureType::Dynamic);
    interface.outputs.fill(
        nodes::StructureTypeInterface::OutputDependency{StructureType::Dynamic});
    return interface;
  }

  for (const int i : input_sockets.index_range()) {
    const nodes::SocketDeclaration &decl = *input_sockets[i]->runtime->declaration;
    interface.inputs[i] = decl.structure_type;
  }

  for (const int output : output_sockets.index_range()) {
    const nodes::SocketDeclaration &decl = *output_sockets[output]->runtime->declaration;
    interface.outputs[output].type = decl.structure_type;
    if (interface.outputs[output].type != StructureType::Dynamic) {
      continue;
    }

    /* Currently the input sockets that influence the field status of an output are the same as the
     * sockets that influence its structure type. Reuse that for the propagation of structure type
     * until there is a more generic format of intra-node dependencies. */
    interface.outputs[output].linked_inputs = decl.output_field_dependency.linked_input_indices();
  }

  return interface;
}

static Array<nodes::StructureTypeInterface> calc_node_interfaces(const bNodeTree &tree)
{
  const Span<const bNode *> nodes = tree.all_nodes();
  Array<nodes::StructureTypeInterface> interfaces(nodes.size());
  for (const int i : nodes.index_range()) {
    interfaces[i] = calc_node_interface(*nodes[i]);
  }
  return interfaces;
}

struct SocketStatus {
  bool is_single = false;
  bool is_grid = false;
  bool is_field = false;

  void merge(const SocketStatus &other, const bool do_grid = true)
  {
    this->is_single |= other.is_single;
    if (do_grid) {
      this->is_grid |= other.is_grid;
    }
    this->is_field |= other.is_field;
  }
};

static void initialize_usages_from_socket_declarations(const bNodeTree &tree,
                                                       MutableSpan<SocketStatus> socket_usages)
{
  for (const bNodeSocket *socket : tree.all_sockets()) {
    const nodes::SocketDeclaration *declaration = socket->runtime->declaration;
    if (!socket->runtime->declaration) {
      continue;
    }
    switch (declaration->structure_type) {
      case StructureType::Dynamic: {
        break;
      }
      case StructureType::Single: {
        socket_usages[socket->index_in_tree()].is_single = true;
        break;
      }
      case StructureType::Grid: {
        socket_usages[socket->index_in_tree()].is_grid = true;
        break;
      }
      case StructureType::Field: {
        socket_usages[socket->index_in_tree()].is_field = true;
        break;
      }
    }
  }
}

static void store_group_input_structure_types(const bNodeTree &tree,
                                              const Span<SocketStatus> socket_usages,
                                              nodes::StructureTypeInterface &derived_interface)
{
  /* Merge usages from all group input nodes. */
  Array<SocketStatus> group_input_usages(tree.interface_inputs().size());
  for (const bNode *node : tree.group_input_nodes()) {
    for (const bNodeSocket *socket : node->output_sockets().drop_back(1)) {
      group_input_usages[socket->index()].merge(socket_usages[socket->index_in_tree()]);
    }
  }

  /* Build derived interface structure types from group input nodes. */
  for (const int input_i : tree.interface_inputs().index_range()) {
    const bNodeTreeInterfaceSocket &io_socket = *tree.interface_inputs()[input_i];
    if (io_socket.structure_type != NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
      derived_interface.inputs[input_i] = StructureType(io_socket.structure_type);
      continue;
    }

    const SocketStatus &usage = group_input_usages[input_i];
    if (usage.is_single) {
      derived_interface.inputs[input_i] = StructureType::Single;
    }
    else if (usage.is_grid) {
      derived_interface.inputs[input_i] = StructureType::Grid;
    }
    else if (usage.is_field) {
      derived_interface.inputs[input_i] = StructureType::Field;
    }
    else {
      derived_interface.inputs[input_i] = StructureType::Dynamic;
    }
  }
}

/** Result of syncing two structure type states. */
enum class StateSyncResult : int8_t {
  /* Nothing changed. */
  NONE = 0,
  /* State A has been modified. */
  CHANGED_A = (1 << 0),
  /* State B has been modified. */
  CHANGED_B = (1 << 1),
};
ENUM_OPERATORS(StateSyncResult, StateSyncResult::CHANGED_B)

/**
 * Compare both states and select the most compatible.
 * Afterwards both states will be the same.
 * \return StateSyncResult flags indicating which states have changed.
 */
static StateSyncResult sync_states(SocketStatus &a, SocketStatus &b)
{
  const bool is_single = a.is_single || b.is_single;

  StateSyncResult res = StateSyncResult::NONE;
  if (a.is_single != is_single) {
    res |= StateSyncResult::CHANGED_A;
  }
  if (b.is_single != is_single) {
    res |= StateSyncResult::CHANGED_B;
  }

  a.is_single = is_single;
  b.is_single = is_single;

  return res;
}

/**
 * Compare states of simulation nodes sockets and select the most compatible.
 * Afterwards all states will be the same.
 * \return StateSyncResult flags indicating which states have changed.
 */
static StateSyncResult simulation_nodes_state_sync(
    const bNode &input_node,
    const bNode &output_node,
    const MutableSpan<SocketStatus> state_by_socket_id)
{
  StateSyncResult res = StateSyncResult::NONE;
  for (const int i : output_node.output_sockets().index_range()) {
    /* First input node output is Delta Time which does not appear in the output node outputs. */
    const bNodeSocket &input_socket = input_node.output_socket(i + 1);
    const bNodeSocket &output_socket = output_node.output_socket(i);
    SocketStatus &input_state = state_by_socket_id[input_socket.index_in_tree()];
    SocketStatus &output_state = state_by_socket_id[output_socket.index_in_tree()];
    res |= sync_states(input_state, output_state);
  }
  return res;
}

static StateSyncResult repeat_state_sync(const bNode &input_node,
                                         const bNode &output_node,
                                         const MutableSpan<SocketStatus> state_by_socket_id)
{
  StateSyncResult res = StateSyncResult::NONE;
  const auto &storage = *static_cast<const NodeGeometryRepeatOutput *>(output_node.storage);
  for (const int i : IndexRange(storage.items_num)) {
    const bNodeSocket &input_socket = input_node.output_socket(i + 1);
    const bNodeSocket &output_socket = output_node.output_socket(i);
    SocketStatus &input_state = state_by_socket_id[input_socket.index_in_tree()];
    SocketStatus &output_state = state_by_socket_id[output_socket.index_in_tree()];
    res |= sync_states(input_state, output_state);
  }
  return res;
}

static bool propagate_special_data_requirements(const bNodeTree &tree,
                                                const bNode &node,
                                                const MutableSpan<SocketStatus> state_by_socket_id)
{
  bool need_update = false;

  /* Sync field state between zone nodes and schedule another pass if necessary. */
  switch (node.type_legacy) {
    case GEO_NODE_SIMULATION_INPUT: {
      const auto &data = *static_cast<const NodeGeometrySimulationInput *>(node.storage);
      if (const bNode *output_node = tree.node_by_id(data.output_node_id)) {
        const StateSyncResult sync_result = simulation_nodes_state_sync(
            node, *output_node, state_by_socket_id);
        if (bool(sync_result & StateSyncResult::CHANGED_B)) {
          need_update = true;
        }
      }
      break;
    }
    case GEO_NODE_SIMULATION_OUTPUT: {
      for (const bNode *input_node : tree.nodes_by_type("GeometryNodeSimulationInput")) {
        const auto &data = *static_cast<const NodeGeometrySimulationInput *>(input_node->storage);
        if (node.identifier == data.output_node_id) {
          const StateSyncResult sync_result = simulation_nodes_state_sync(
              *input_node, node, state_by_socket_id);
          if (bool(sync_result & StateSyncResult::CHANGED_A)) {
            need_update = true;
          }
        }
      }
      break;
    }
    case GEO_NODE_REPEAT_INPUT: {
      const auto &data = *static_cast<const NodeGeometryRepeatInput *>(node.storage);
      if (const bNode *output_node = tree.node_by_id(data.output_node_id)) {
        const StateSyncResult sync_result = repeat_state_sync(
            node, *output_node, state_by_socket_id);
        if (bool(sync_result & StateSyncResult::CHANGED_B)) {
          need_update = true;
        }
      }
      break;
    }
    case GEO_NODE_REPEAT_OUTPUT: {
      for (const bNode *input_node : tree.nodes_by_type("GeometryNodeRepeatInput")) {
        const auto &data = *static_cast<const NodeGeometryRepeatInput *>(input_node->storage);
        if (node.identifier == data.output_node_id) {
          const StateSyncResult sync_result = repeat_state_sync(
              *input_node, node, state_by_socket_id);
          if (bool(sync_result & StateSyncResult::CHANGED_A)) {
            need_update = true;
          }
        }
      }
      break;
    }
  }

  return need_update;
}

static void propagate_right_to_left(const bNodeTree &tree,
                                    const Span<nodes::StructureTypeInterface> node_interfaces,
                                    MutableSpan<SocketStatus> socket_usages)
{
  while (true) {
    bool need_update = false;

    for (const bNode *node : tree.toposort_right_to_left()) {
      if (node->is_group_output()) {
        /* The output is not constrained. */
        continue;
      }

      /* Constraint outputs based on where they are connected. */
      for (const bNodeSocket *output_socket : node->output_sockets()) {
        SocketStatus &output_usage = socket_usages[output_socket->index_in_tree()];
        for (const bNodeLink *link : output_socket->directly_linked_links()) {
          if (!link->is_used()) {
            continue;
          }
          const bNodeSocket &target_socket = *link->tosock;
          output_usage.merge(socket_usages[target_socket.index_in_tree()]);
        }
      }

      /* Propagate contraints from node outputs to inputs. */

      if (node->is_reroute()) {
        const int input = node->input_socket(0).index_in_tree();
        const int output = node->output_socket(0).index_in_tree();
        socket_usages[input] = socket_usages[output];
        continue;
      }

      const nodes::StructureTypeInterface &interface = node_interfaces[node->index()];
      for (const int output_index : interface.outputs.index_range()) {
        const bNodeSocket &output = node->output_socket(output_index);
        if (!output.is_available()) {
          continue;
        }
        for (const int input_index : interface.outputs[output_index].linked_inputs) {
          const bNodeSocket &input = node->input_socket(input_index);
          if (!input.is_available() || !output.is_available()) {
            continue;
          }
          socket_usages[input.index_in_tree()].merge(socket_usages[output.index_in_tree()]);
        }
      }

      /* Find reverse dependencies and resolve conflicts, which may require another pass. */
      if (propagate_special_data_requirements(tree, *node, socket_usages)) {
        need_update = true;
      }
    }

    if (!need_update) {
      break;
    }
  }
}

static void propagate_left_to_right(const bNodeTree &tree,
                                    const Span<nodes::StructureTypeInterface> node_interfaces,
                                    MutableSpan<SocketStatus> socket_usages)
{
  while (true) {
    bool need_update = false;
    for (const bNode *node : tree.toposort_left_to_right()) {
      for (const bNodeSocket *input_socket : node->input_sockets()) {
        if (!input_socket->is_available()) {
          continue;
        }
        SocketStatus &socket_structure_type = socket_usages[input_socket->index_in_tree()];
        if (input_socket->is_directly_linked()) {
          const bNodeLink &link = *input_socket->directly_linked_links()[0];
          if (link.is_used()) {
            socket_structure_type = socket_usages[link.fromsock->index_in_tree()];
            continue;
          }
        }
      }

      if (node->is_group_input()) {
        continue;
      }

      if (node->is_undefined()) {
        continue;
      }

      if (node->is_reroute()) {
        const int input = node->input_socket(0).index_in_tree();
        const int output = node->output_socket(0).index_in_tree();
        socket_usages[output] = socket_usages[input];
        continue;
      }

      const nodes::StructureTypeInterface &interface = node_interfaces[node->index()];

      for (const int output_index : interface.outputs.index_range()) {
        const bNodeSocket &output = node->output_socket(output_index);
        if (!output.is_available()) {
          continue;
        }
        if (output.runtime->declaration->structure_type != StructureType::Dynamic) {
          continue;
        }
        for (const int input_index : interface.outputs[output_index].linked_inputs) {
          const bNodeSocket &input = node->input_socket(input_index);
          if (!input.is_available()) {
            continue;
          }
          socket_usages[output.index_in_tree()].merge(socket_usages[input.index_in_tree()]);
        }
      }

      /* Find reverse dependencies and resolve conflicts, which may require another pass. */
      if (propagate_special_data_requirements(tree, *node, socket_usages)) {
        need_update = true;
      }
    }

    if (!need_update) {
      break;
    }
  }
}

static Vector<int> find_dynamic_output_linked_inputs(
    const bNodeSocket &group_output, const Span<nodes::StructureTypeInterface> interface_by_node)
{
  /* Use a Set instead of an array indexed by socket because we may only look at a few sockets. */
  Set<const bNodeSocket *> handled_sockets;
  Stack<const bNodeSocket *> sockets_to_check;

  handled_sockets.add(&group_output);
  sockets_to_check.push(&group_output);

  Vector<int> group_inputs;

  while (!sockets_to_check.is_empty()) {
    const bNodeSocket *input_socket = sockets_to_check.pop();
    if (!input_socket->is_directly_linked()) {
      continue;
    }

    for (const bNodeSocket *origin_socket : input_socket->directly_linked_sockets()) {
      const bNode &origin_node = origin_socket->owner_node();
      if (origin_node.is_group_input()) {
        group_inputs.append_non_duplicates(origin_socket->index());
        continue;
      }

      const nodes::StructureTypeInterface &interface = interface_by_node[origin_node.index()];
      for (const int input_index : interface.outputs[origin_socket->index()].linked_inputs) {
        const bNodeSocket &input = origin_node.input_socket(input_index);
        if (!input.is_available()) {
          continue;
        }
        if (handled_sockets.add(&input)) {
          sockets_to_check.push(&input);
        }
      }
    }
  }

  return group_inputs;
}

static void store_group_output_structure_types(
    const bNodeTree &tree,
    const Span<nodes::StructureTypeInterface> interface_by_node,
    const Span<SocketStatus> socket_usages,
    nodes::StructureTypeInterface &interface)
{
  const bNode *group_output_node = tree.group_output_node();
  if (!group_output_node) {
    return;
  }

  const Span<const bNodeTreeInterfaceSocket *> interface_outputs = tree.interface_outputs();
  const Span<const bNodeSocket *> sockets = group_output_node->input_sockets().drop_back(1);
  for (const int i : sockets.index_range()) {
    if (interface_outputs[i]->structure_type != NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
      interface.outputs[i] = {StructureType(interface_outputs[i]->structure_type), {}};
      continue;
    }
    /* Update derived interface output structure types from output node socket usages. */
    const SocketStatus usage = socket_usages[sockets[i]->index_in_tree()];
    if (usage.is_grid) {
      interface.outputs[i] = {StructureType::Grid, {}};
      continue;
    }
    if (usage.is_single) {
      interface.outputs[i] = {StructureType::Single, {}};
      continue;
    }
    if (usage.is_field) {
      interface.outputs[i] = {StructureType::Field, {}};
      continue;
    }
    const Vector<int> linked_inputs = find_dynamic_output_linked_inputs(*sockets[i],
                                                                        interface_by_node);
    interface.outputs[i] = {StructureType::Dynamic, linked_inputs.as_span()};
  }
}

static std::unique_ptr<nodes::StructureTypeInterface> calc_structure_type_interface(
    const bNodeTree &tree)
{
  tree.ensure_topology_cache();
  tree.ensure_interface_cache();
  if (tree.has_available_link_cycle()) {
    return {};
  }

  Array<nodes::StructureTypeInterface> node_interfaces = calc_node_interfaces(tree);

  auto derived_interface = std::make_unique<nodes::StructureTypeInterface>();
  derived_interface->inputs.reinitialize(tree.interface_inputs().size());
  derived_interface->outputs.reinitialize(tree.interface_outputs().size());

  Array<SocketStatus> socket_usages(tree.all_sockets().size());

  initialize_usages_from_socket_declarations(tree, socket_usages);
  propagate_right_to_left(tree, node_interfaces, socket_usages);
  store_group_input_structure_types(tree, socket_usages, *derived_interface);
  propagate_left_to_right(tree, node_interfaces, socket_usages);
  store_group_output_structure_types(tree, node_interfaces, socket_usages, *derived_interface);

  return derived_interface;
}

bool update_structure_type_interface(bNodeTree &tree)
{
  std::unique_ptr<nodes::StructureTypeInterface> new_interface = calc_structure_type_interface(
      tree);
  if (tree.runtime->structure_type_interface &&
      *tree.runtime->structure_type_interface == *new_interface)
  {
    return false;
  }
  tree.runtime->structure_type_interface = std::move(new_interface);
  return true;
}

}  // namespace blender::bke::node_structure_type_inferencing
