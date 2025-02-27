/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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
  nodes::StructureTypeInterface interface;

  const Span<const bNodeSocket *> input_sockets = node.input_sockets();
  interface.inputs.reinitialize(input_sockets.size());
  for (const int i : input_sockets.index_range()) {
    const nodes::SocketDeclaration &decl = *input_sockets[i]->runtime->declaration;
    interface.inputs[i] = decl.structure_type;
  }

  const Span<const bNodeSocket *> output_sockets = node.output_sockets();
  interface.outputs.reinitialize(output_sockets.size());
  interface.output_input_dependencies.reinitialize(output_sockets.size());
  for (const int output : output_sockets.index_range()) {
    const nodes::SocketDeclaration &decl = *output_sockets[output]->runtime->declaration;
    interface.outputs[output] = decl.structure_type;
    if (interface.outputs[output] != StructureType::Dynamic) {
      continue;
    }

    /* Currently the input sockets that influence the field status of an output are the same as the
     * sockets that influence its structure type. Reuse that for the propagation of structure type
     * until there is a more generic format of intra-node dependencies. */
    const Span<int> dependent_inputs = decl.output_field_dependency.linked_input_indices();
    interface.output_input_dependencies[output] = dependent_inputs;
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
  bool is_single_value = false;
  bool is_grid = false;
  bool is_field = false;

  void merge(const SocketStatus &other, const bool do_grid = true)
  {
    this->is_single_value |= other.is_single_value;
    if (do_grid) {
      this->is_grid |= other.is_grid;
    }
    this->is_field |= other.is_field;
  }
};

static void initialize_usages_from_socket_declarations(const bNodeTree &tree,
                                                       MutableSpan<SocketStatus> socket_usages)
{
  // TODO: There needs to be a difference between NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO and
  // NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_DYNAMIC
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
        socket_usages[socket->index_in_tree()].is_single_value = true;
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

static void update_interface_structure_types(const bNodeTree &tree,
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
    if (usage.is_single_value) {
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

  /* Update derived interface output structure types from output node socket usages. */
  if (const bNode *output_node = tree.group_output_node()) {
    for (const int output_i : tree.interface_outputs().index_range()) {
      const bNodeTreeInterfaceSocket &io_socket = *tree.interface_outputs()[output_i];
      if (io_socket.structure_type != NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
        derived_interface.outputs[output_i] = StructureType(io_socket.structure_type);
        continue;
      }
      const SocketStatus &usage =
          socket_usages[output_node->input_socket(output_i).index_in_tree()];
      if (usage.is_single_value) {
        derived_interface.outputs[output_i] = StructureType::Single;
      }
      else if (usage.is_grid) {
        derived_interface.outputs[output_i] = StructureType::Grid;
      }
      else if (usage.is_field) {
        derived_interface.outputs[output_i] = StructureType::Field;
      }
      else {
        derived_interface.outputs[output_i] = StructureType::Dynamic;
      }
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
 * Compare both field states and select the most compatible.
 * Afterwards both field states will be the same.
 * \return StateSyncResult flags indicating which field states have changed.
 */
static StateSyncResult sync_field_states(SocketStatus &a, SocketStatus &b)
{
  const bool is_single = a.is_single_value || b.is_single_value;

  StateSyncResult res = StateSyncResult::NONE;
  if (a.is_single_value != is_single) {
    res |= StateSyncResult::CHANGED_A;
  }
  if (b.is_single_value != is_single) {
    res |= StateSyncResult::CHANGED_B;
  }

  a.is_single_value = is_single;
  b.is_single_value = is_single;

  return res;
}

/**
 * Compare field states of simulation nodes sockets and select the most compatible.
 * Afterwards all field states will be the same.
 * \return StateSyncResult flags indicating which field states have changed.
 */
static StateSyncResult simulation_nodes_field_state_sync(
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
    res |= sync_field_states(input_state, output_state);
  }
  return res;
}

static StateSyncResult repeat_field_state_sync(const bNode &input_node,
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
    res |= sync_field_states(input_state, output_state);
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
        const StateSyncResult sync_result = simulation_nodes_field_state_sync(
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
          const StateSyncResult sync_result = simulation_nodes_field_state_sync(
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
        const StateSyncResult sync_result = repeat_field_state_sync(
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
          const StateSyncResult sync_result = repeat_field_state_sync(
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
                                    MutableSpan<SocketStatus> socket_usages,
                                    nodes::StructureTypeInterface &derived_interface)
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
      for (const int output_index : interface.output_input_dependencies.index_range()) {
        const bNodeSocket &output = node->output_socket(output_index);
        if (!output.is_available()) {
          continue;
        }
        for (const int input_index : interface.output_input_dependencies[output_index]) {
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

  update_interface_structure_types(tree, socket_usages, derived_interface);
}

static void propagate_left_to_right(const bNodeTree &tree,
                                    const Span<nodes::StructureTypeInterface> node_interfaces,
                                    MutableSpan<SocketStatus> socket_usages,
                                    nodes::StructureTypeInterface &derived_interface)
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
        else if (input_socket->runtime->declaration) {
          if (input_socket->runtime->declaration->input_field_type ==
              nodes::InputSocketFieldType::Implicit)
          {
            socket_structure_type.is_field = true;
            continue;
          }
        }
      }

      if (node->is_group_input()) {
        continue;
      }

      if (node->is_reroute()) {
        const int input = node->input_socket(0).index_in_tree();
        const int output = node->output_socket(0).index_in_tree();
        socket_usages[output] = socket_usages[input];
        continue;
      }

      const nodes::StructureTypeInterface &interface = node_interfaces[node->index()];

      for (const int output_index : interface.output_input_dependencies.index_range()) {
        const bNodeSocket &output = node->output_socket(output_index);
        if (!output.is_available()) {
          continue;
        }
        if (output.runtime->declaration->structure_type != StructureType::Dynamic) {
          continue;
        }
        for (const int input_index : interface.output_input_dependencies[output_index]) {
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

  update_interface_structure_types(tree, socket_usages, derived_interface);
}

static std::unique_ptr<nodes::StructureTypeInterface> calc_structure_type_interface(
    const bNodeTree &tree)
{
  tree.ensure_topology_cache();
  tree.ensure_interface_cache();
  if (tree.has_available_link_cycle()) {
    return {};
  }

  std::unique_ptr<nodes::StructureTypeInterface> derived_interface =
      std::make_unique<nodes::StructureTypeInterface>();
  derived_interface->inputs.reinitialize(tree.interface_inputs().size());
  derived_interface->outputs.reinitialize(tree.interface_outputs().size());

  Array<SocketStatus> socket_usages(tree.all_sockets().size());

  Array<nodes::StructureTypeInterface> node_interfaces = calc_node_interfaces(tree);

  initialize_usages_from_socket_declarations(tree, socket_usages);
  propagate_right_to_left(tree, node_interfaces, socket_usages, *derived_interface);
  propagate_left_to_right(tree, node_interfaces, socket_usages, *derived_interface);

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
