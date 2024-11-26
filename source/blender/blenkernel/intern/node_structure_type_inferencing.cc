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

bool update_structure_type_inferencing(bNodeTree &tree)
{
  tree.ensure_topology_cache();
  tree.ensure_interface_cache();
  if (tree.has_available_link_cycle()) {
    return true;
  }

  ResourceScope scope;
  Array<const nodes::anonymous_attribute_lifetime::RelationsInNode *> relations_by_node =
      node_tree_reference_lifetimes::prepare_relations_by_node(tree, scope);

  Array<SocketUsageInfo> socket_usages(tree.all_sockets().size());
  /* TODO: Handle zones. */
  for (const bNode *node : tree.toposort_right_to_left()) {
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

    switch (node->type) {
      case NODE_REROUTE: {
        socket_usages[node->input_socket(0).index_in_tree()] =
            socket_usages[node->output_socket(0).index_in_tree()];
        break;
      }
      case NODE_GROUP_OUTPUT: {
        /* The output is not constrained. */
        break;
      }
      default: {
        const nodes::aal::RelationsInNode *relations = relations_by_node[node->index()];
        if (!relations) {
          break;
        }
        for (const nodes::aal::ReferenceRelation &relation : relations->reference_relations) {
          const bNodeSocket &input_socket = node->input_socket(relation.from_field_input);
          const bNodeSocket &output_socket = node->output_socket(relation.to_field_output);
          if (!input_socket.is_available() || !output_socket.is_available()) {
            continue;
          }
          socket_usages[input_socket.index_in_tree()].merge(
              socket_usages[output_socket.index_in_tree()], false);
        }
        for (const bNodeSocket *input_socket : node->input_sockets()) {
          if (!input_socket->is_available()) {
            continue;
          }
          if (!input_socket->runtime->declaration) {
            continue;
          }
          SocketUsageInfo &usage = socket_usages[input_socket->index_in_tree()];
          switch (input_socket->runtime->declaration->structure_type) {
            case StructureType::Field: {
              usage.evaluated_as_field = true;
              break;
            }
            case StructureType::Single: {
              usage.requires_single_value = true;
              break;
            }
            case StructureType::Grid: {
              usage.requires_grid = true;
              break;
            }
            default: {
              break;
            }
          }
        }
        break;
      }
    }
  }

  /* TODO */
  bool interface_changed = true;

  Vector<SocketUsageInfo> group_input_usages(tree.interface_inputs().size());
  for (const bNode *node : tree.group_input_nodes()) {
    for (const bNodeSocket *socket : node->output_sockets().drop_back(1)) {
      group_input_usages[socket->index()].merge(socket_usages[socket->index_in_tree()]);
    }
  }

  Array<StructureType> socket_structure_types(tree.all_sockets().size(), StructureType::Dynamic);

  for (const int input_i : tree.interface_inputs().index_range()) {
    bNodeTreeInterfaceSocket &io_socket = *tree.interface_inputs()[input_i];
    if (io_socket.structure_type == NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
      const SocketUsageInfo &usage = group_input_usages[input_i];
      if (usage.requires_single_value) {
        io_socket.derived_structure_type = int8_t(StructureType::Single);
      }
      else if (usage.evaluated_as_field) {
        io_socket.derived_structure_type = int8_t(StructureType::Field);
      }
      else if (usage.requires_grid) {
        io_socket.derived_structure_type = int8_t(StructureType::Grid);
      }
      else {
        io_socket.derived_structure_type = int8_t(StructureType::Dynamic);
      }
    }
    else {
      io_socket.derived_structure_type = int8_t(StructureType(io_socket.structure_type));
    }
    for (bNode *node : tree.group_input_nodes()) {
      const StructureType derived_structure_type = StructureType(io_socket.derived_structure_type);
      bNodeSocket &socket = node->output_socket(input_i);
      const_cast<StructureType &>(
          socket.runtime->declaration->structure_type) = derived_structure_type;
      socket_structure_types[socket.index_in_tree()] = derived_structure_type;
    }
  }

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

  if (bNode *output_node = tree.group_output_node()) {
    for (const int output_i : tree.interface_outputs().index_range()) {
      const bNodeSocket &socket = output_node->input_socket(output_i);
      bNodeTreeInterfaceSocket &io_socket = *tree.interface_outputs()[output_i];
      const StructureType structure_type = socket_structure_types[socket.index_in_tree()];
      io_socket.derived_structure_type = int8_t(structure_type);
    }
  }
  for (bNode *output_node : tree.group_output_nodes()) {
    for (const int output_i : tree.interface_outputs().index_range()) {
      bNodeSocket &socket = output_node->input_socket(output_i);
      const bNodeTreeInterfaceSocket &io_socket = *tree.interface_outputs()[output_i];
      const StructureType derived_structure_type = StructureType(io_socket.derived_structure_type);
      socket_structure_types[socket.index_in_tree()] = derived_structure_type;
      const_cast<StructureType &>(
          socket.runtime->declaration->structure_type) = derived_structure_type;
    }
  }

  for (bNodeSocket *socket : tree.all_sockets()) {
    const StructureType type = socket_structure_types[socket->index_in_tree()];
    socket->runtime->field_state = ELEM(type, StructureType::Dynamic, StructureType::Field) &&
                                           nodes::socket_type_supports_fields(
                                               eNodeSocketDatatype(socket->type)) ?
                                       std::make_optional(FieldSocketState::IsField) :
                                       std::nullopt;
  }

  return interface_changed;
}

}  // namespace blender::bke::node_structure_type_inferencing
