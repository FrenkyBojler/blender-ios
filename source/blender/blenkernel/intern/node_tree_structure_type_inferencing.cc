/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_reference_lifetimes.hh"

#include "NOD_geometry.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket.hh"

#include "BLI_resource_scope.hh"
#include "BLI_set.hh"
#include "BLI_stack.hh"

namespace blender::bke::node_structure_type_inferencing {

using nodes::StructureType;
namespace aal = nodes::anonymous_attribute_lifetime;

struct SocketUsageInfo {
  bool requires_single_value = false;
  bool requires_grid = false;
  bool evaluated_as_field = false;

  void merge(const SocketUsageInfo &other, const bool do_grid = true)
  {
    this->requires_single_value |= other.requires_single_value;
    if (do_grid) {
      this->requires_grid |= other.requires_grid;
    }
    this->evaluated_as_field |= other.evaluated_as_field;
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
        socket_usages[socket->index_in_tree()].requires_single_value = true;
        break;
      }
      case StructureType::Grid: {
        socket_usages[socket->index_in_tree()].requires_grid = true;
        break;
      }
      case StructureType::Field: {
        socket_usages[socket->index_in_tree()].evaluated_as_field = true;
        break;
      }
    }
  }
}

static void update_interface_structure_types(
    const bNodeTree &tree,
    const Span<SocketUsageInfo> socket_usages,
    nodes::StructureTypeInferencingInterface &derived_interface)
{
  /* Merge usages from all group input nodes. */
  Array<SocketUsageInfo> group_input_usages(tree.interface_inputs().size());
  for (const bNode *node : tree.group_input_nodes()) {
    for (const bNodeSocket *socket : node->output_sockets().drop_back(1)) {
      group_input_usages[socket->index()].merge(socket_usages[socket->index_in_tree()]);
    }
  }

  /* Build derived inputs from group input nodes. */
  for (const int input_i : tree.interface_inputs().index_range()) {
    bNodeTreeInterfaceSocket &io_socket = *tree.interface_inputs()[input_i];
    if (io_socket.structure_type != NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
      derived_interface.inputs[input_i] = StructureType(io_socket.structure_type);
      continue;
    }

    const SocketUsageInfo &usage = group_input_usages[input_i];
    if (usage.requires_single_value) {
      derived_interface.inputs[input_i] = StructureType::Single;
    }
    else if (usage.evaluated_as_field) {
      derived_interface.inputs[input_i] = StructureType::Field;
    }
    else if (usage.requires_grid) {
      derived_interface.inputs[input_i] = StructureType::Grid;
    }
    else {
      derived_interface.inputs[input_i] = StructureType::Dynamic;
    }
  }

  if (bNode *output_node = tree.group_output_node()) {
    for (const int output_i : tree.interface_outputs().index_range()) {
      const bNodeSocket &socket = output_node->input_socket(output_i);
      bNodeTreeInterfaceSocket &io_socket = *tree.interface_outputs()[output_i];
      if (io_socket.structure_type != NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
        derived_interface.outputs[output_i] = StructureType(io_socket.structure_type);
        continue;
      }
      const SocketUsageInfo &usage =
          socket_usages[output_node->input_socket(output_i).index_in_tree()];
      if (usage.requires_single_value) {
        derived_interface.outputs[output_i] = StructureType::Single;
      }
      else if (usage.evaluated_as_field) {
        derived_interface.outputs[output_i] = StructureType::Field;
      }
      else if (usage.requires_grid) {
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
    nodes::StructureTypeInferencingInterface &derived_interface)
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
      socket_usages[node->input_socket(0).index_in_tree()] =
          socket_usages[node->output_socket(0).index_in_tree()];
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
      // TODO: Why not merging the `requires_grid` here?
      socket_usages[input_socket.index_in_tree()].merge(
          socket_usages[output_socket.index_in_tree()], false);
    }
  }

  update_interface_structure_types(tree, socket_usages, derived_interface);
}

static void propagate_left_to_right(const bNodeTree &tree,
                                    MutableSpan<SocketUsageInfo> socket_usages,
                                    nodes::StructureTypeInferencingInterface &derived_interface)
{
  for (const bNode *node : tree.toposort_left_to_right()) {
    for (const bNodeSocket *input_socket : node->input_sockets()) {
      if (!input_socket->is_available()) {
        continue;
      }
      StructureType &socket_structure_type = socket_structure_types[input_socket->index_in_tree()];
      if (input_socket->is_directly_linked()) {
        const bNodeLink &link = *input_socket->directly_linked_links()[0];
        if (link.is_used()) {
          socket_structure_type = socket_structure_types[link.fromsock->index_in_tree()];
          continue;
        }
      }
      else if (input_socket->runtime->declaration) {
        if (input_socket->runtime->declaration->input_field_type ==
            nodes::InputSocketFieldType::Implicit)
        {
          socket_structure_type = StructureType::Field;
          continue;
        }
      }
      socket_structure_type = StructureType::Single;
    }

    switch (node->type) {
      case NODE_REROUTE: {
        socket_structure_types[node->output_socket(0).index_in_tree()] =
            socket_structure_types[node->input_socket(0).index_in_tree()];
        break;
      }
      case NODE_GROUP_INPUT: {
        /* Done already. */
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
          socket_structure_types[output_socket.index_in_tree()] = StructureType::Single;
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
          StructureType &output_structure_type =
              socket_structure_types[output_socket.index_in_tree()];
          const StructureType input_structure_type =
              socket_structure_types[input_socket.index_in_tree()];
          if (input_structure_type == StructureType::Dynamic && ELEM(output_structure_type,
                                                                     StructureType::Single,
                                                                     StructureType::Field,
                                                                     StructureType::Grid))
          {
            output_structure_type = StructureType::Dynamic;
          }
          else if (input_structure_type == StructureType::Grid &&
                   ELEM(output_structure_type, StructureType::Single, StructureType::Field))
          {
            output_structure_type = StructureType::Grid;
          }
          else if (input_structure_type == StructureType::Field &&
                   output_structure_type == StructureType::Single)
          {
            output_structure_type = StructureType::Field;
          }
        }
        for (const bNodeSocket *output_socket : node->output_sockets()) {
          if (!output_socket->is_available()) {
            continue;
          }
          if (output_socket->runtime->declaration) {
            if (output_socket->runtime->declaration->structure_type != StructureType::Dynamic) {
              socket_structure_types[output_socket->index_in_tree()] =
                  output_socket->runtime->declaration->structure_type;
            }
          }
        }
        break;
      }
    }
  }

  update_interface_structure_types(tree, socket_usages, derived_interface);
}

bool update_structure_type_inferencing(const bNodeTree &tree)
{
  tree.ensure_topology_cache();
  tree.ensure_interface_cache();
  if (tree.has_available_link_cycle()) {
    return true;
  }

  nodes::StructureTypeInferencingInterface derived_interface;
  derived_interface.inputs.reinitialize(tree.interface_inputs().size());
  derived_interface.outputs.reinitialize(tree.interface_outputs().size());

  Array<SocketUsageInfo> socket_usages(tree.all_sockets().size());

  ResourceScope scope;
  Array<const nodes::anonymous_attribute_lifetime::RelationsInNode *> relations_by_node =
      node_tree_reference_lifetimes::prepare_relations_by_node(tree, scope);

  initialize_usages_from_socket_declarations(tree, socket_usages);
  propagate_right_to_left(tree, relations_by_node, socket_usages, derived_interface);
  propagate_left_to_right(tree, socket_usages, derived_interface);

  /* TODO: Handle zones. */
  /* TODO */
  bool interface_changed = true;

  return interface_changed;
}

}  // namespace blender::bke::node_structure_type_inferencing
