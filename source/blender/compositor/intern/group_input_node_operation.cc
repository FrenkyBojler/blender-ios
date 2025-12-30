/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_node_types.h"

#include "COM_context.hh"
#include "COM_group_input_node_operation.hh"
#include "COM_node_group_operation.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

/* TODO. */
class GroupInputNodeOperation : public NodeOperation {
 private:
  NodeGroupOperation &node_group_operation_;

 public:
  GroupInputNodeOperation(Context &context,
                          const bNode &node,
                          NodeGroupOperation &node_group_operation)
      : NodeOperation(context, node), node_group_operation_(node_group_operation)
  {
  }

  void execute() override
  {
    for (const bNodeSocket *output_socket : this->node().output_sockets()) {
      if (!is_socket_available(output_socket)) {
        continue;
      }

      const Result &node_group_operation_input = node_group_operation_.get_input(
          output_socket->identifier);
      Result &output_result = this->get_result(output_socket->identifier);
      output_result.share_data(node_group_operation_input);
    }
  }
};

NodeOperation *get_group_input_node_operation(Context &context,
                                              const bNode &node,
                                              NodeGroupOperation &node_group_operation)
{
  return new GroupInputNodeOperation(context, node, node_group_operation);
}

}  // namespace blender::compositor
