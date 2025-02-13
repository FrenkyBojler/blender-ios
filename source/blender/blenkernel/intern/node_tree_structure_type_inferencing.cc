/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_reference_lifetimes.hh"

#include "DNA_node_tree_interface_types.h"
#include "NOD_node_declaration.hh"

#include "BLI_resource_scope.hh"

namespace blender::bke::node_structure_type_inferencing {

using nodes::StructureType;
namespace aal = nodes::anonymous_attribute_lifetime;

struct SocketUsageInfo {
  bool is_single_value = false;
  bool is_grid = false;
  bool is_field = false;

  void merge(const SocketUsageInfo &other, const bool do_grid = true)
  {
    this->is_single_value |= other.is_single_value;
    if (do_grid) {
      this->is_grid |= other.is_grid;
    }
    this->is_field |= other.is_field;
  }
};

static void initialize_usages_from_socket_declarations(const bNodeTree &tree,
                                                       MutableSpan<SocketUsageInfo> socket_usages)
{
  for (const bNodeSocket *socket : tree.all_sockets()) {
    const nodes::SocketDeclaration *declaration = socket->runtime->declaration;
    if (!socket->runtime->declaration) {
      continue;
    }
    const StructureType structure_type = declaration->structure_type;
    switch (structure_type) {
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
                                             const Span<SocketUsageInfo> socket_usages,
                                             nodes::StructureTypeInterface &derived_interface)
{
  /* Merge usages from all group input nodes. */
  Array<SocketUsageInfo> group_input_usages(tree.interface_inputs().size());
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

    const SocketUsageInfo &usage = group_input_usages[input_i];
    if (usage.is_single_value) {
      derived_interface.inputs[input_i] = StructureType::Single;
    }
    else if (usage.is_field) {
      derived_interface.inputs[input_i] = StructureType::Field;
    }
    else if (usage.is_grid) {
      derived_interface.inputs[input_i] = StructureType::Grid;
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
      const SocketUsageInfo &usage =
          socket_usages[output_node->input_socket(output_i).index_in_tree()];
      if (usage.is_single_value) {
        derived_interface.outputs[output_i] = StructureType::Single;
      }
      else if (usage.is_field) {
        derived_interface.outputs[output_i] = StructureType::Field;
      }
      else if (usage.is_grid) {
        derived_interface.outputs[output_i] = StructureType::Grid;
      }
      else {
        derived_interface.outputs[output_i] = StructureType::Dynamic;
      }
    }
  }
}

static void propagate_right_to_left(
    const bNodeTree &tree,
    const Span<const nodes::anonymous_attribute_lifetime::RelationsInNode *> relations_by_node,
    MutableSpan<SocketUsageInfo> socket_usages,
    nodes::StructureTypeInterface &derived_interface)
{
  for (const bNode *node : tree.toposort_right_to_left()) {
    if (node->is_group_output()) {
      /* The output is not constrained. */
      continue;
    }

    /* Constraint outputs based on where they are connected. */
    for (const bNodeSocket *output_socket : node->output_sockets()) {
      SocketUsageInfo &output_usage = socket_usages[output_socket->index_in_tree()];
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

    const nodes::aal::RelationsInNode *relations = relations_by_node[node->index()];
    if (!relations) {
      continue;
    }

    for (const nodes::aal::ReferenceRelation &relation : relations->reference_relations) {
      const bNodeSocket &input_socket = node->input_socket(relation.from_field_input);
      const bNodeSocket &output_socket = node->output_socket(relation.to_field_output);
      if (!input_socket.is_available() || !output_socket.is_available()) {
        continue;
      }
      const int input = input_socket.index_in_tree();
      const int output = output_socket.index_in_tree();
      // Maybe ignore grid requirement when merging?
      socket_usages[input].merge(socket_usages[output]);
    }
  }

  update_interface_structure_types(tree, socket_usages, derived_interface);
}

static void propagate_left_to_right(
    const bNodeTree &tree,
    const Span<const nodes::anonymous_attribute_lifetime::RelationsInNode *> relations_by_node,
    MutableSpan<SocketUsageInfo> socket_usages,
    nodes::StructureTypeInterface &derived_interface)
{
  for (const bNode *node : tree.toposort_left_to_right()) {
    for (const bNodeSocket *input_socket : node->input_sockets()) {
      if (!input_socket->is_available()) {
        continue;
      }
      SocketUsageInfo &socket_structure_type = socket_usages[input_socket->index_in_tree()];
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

    switch (node->type_legacy) {
      case NODE_REROUTE: {
        const int input = node->input_socket(0).index_in_tree();
        const int output = node->output_socket(0).index_in_tree();
        socket_usages[output] = socket_usages[input];
        break;
      }
      case NODE_GROUP_INPUT: {
        break;
      }
      default: {
        const nodes::aal::RelationsInNode *relations = relations_by_node[node->index()];
        if (!relations) {
          break;
        }
        for (const nodes::aal::ReferenceRelation &relation : relations->reference_relations) {
          const bNodeSocket &output_socket = node->output_socket(relation.to_field_output);
          if (!output_socket.is_available()) {
            continue;
          }
          socket_usages[output_socket.index_in_tree()].is_field = true;
        }

        for (const nodes::aal::ReferenceRelation &relation : relations->reference_relations) {
          const bNodeSocket &output_socket = node->output_socket(relation.to_field_output);
          if (!output_socket.is_available()) {
            continue;
          }
          if (output_socket.runtime->declaration) {
            if (output_socket.runtime->declaration->structure_type != StructureType::Dynamic) {
              continue;
            }
          }
          const bNodeSocket &input_socket = node->input_socket(relation.from_field_input);
          if (!input_socket.is_available()) {
            continue;
          }
          const int input = input_socket.index_in_tree();
          const int output = output_socket.index_in_tree();
          socket_usages[output].is_field = socket_usages[input].is_field;
        }

        for (const bNodeSocket *output_socket : node->output_sockets()) {
          if (!output_socket->is_available()) {
            continue;
          }
          if (output_socket->runtime->declaration) {
            if (output_socket->runtime->declaration->structure_type == StructureType::Single) {
              socket_usages[output_socket->index_in_tree()].is_single_value = true;
            }
          }
        }
        break;
      }
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
  derived_interface->all_sockets.reinitialize(tree.all_sockets().size());

  Array<SocketUsageInfo> socket_usages(tree.all_sockets().size());

  ResourceScope scope;
  Array<const nodes::anonymous_attribute_lifetime::RelationsInNode *> relations_by_node =
      node_tree_reference_lifetimes::prepare_relations_by_node(tree, scope);

  initialize_usages_from_socket_declarations(tree, socket_usages);
  propagate_right_to_left(tree, relations_by_node, socket_usages, *derived_interface);
  propagate_left_to_right(tree, relations_by_node, socket_usages, *derived_interface);

  const Span<const bNodeSocket *> sockets = tree.all_sockets();
  for (const int i : sockets.index_range()) {
    derived_interface->all_sockets[i] = StructureType::Dynamic;
    if (socket_usages[i].is_field) {
      derived_interface->all_sockets[i] = StructureType::Field;
    }
    if (socket_usages[i].is_single_value) {
      derived_interface->all_sockets[i] = StructureType::Single;
    }
    if (socket_usages[i].is_grid) {
      derived_interface->all_sockets[i] = StructureType::Grid;
    }
  }

  /* TODO: Handle zones. */

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
