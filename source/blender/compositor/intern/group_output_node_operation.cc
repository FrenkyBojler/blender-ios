/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_node_types.h"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "COM_context.hh"
#include "COM_group_output_node_operation.hh"
#include "COM_node_group_operation.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

/* TODO. */
class GroupOutputNodeOperation : public NodeOperation {
 private:
  NodeGroupOperation &node_group_operation_;

 public:
  GroupOutputNodeOperation(Context &context,
                           const bNode &node,
                           NodeGroupOperation &node_group_operation)
      : NodeOperation(context, node), node_group_operation_(node_group_operation)
  {
    for (const bNodeSocket *input : node.input_sockets()) {
      if (!is_socket_available(input)) {
        continue;
      }

      InputDescriptor &descriptor = this->get_input_descriptor(input->identifier);
      descriptor.expects_single_value = false;
      descriptor.realization_mode = InputRealizationMode::None;
    }
  }

  void execute() override
  {
    for (const bNodeSocket *input_socket : this->node().input_sockets()) {
      if (!is_socket_available(input_socket)) {
        continue;
      }

      Result &node_group_operation_result = node_group_operation_.get_result(
          input_socket->identifier);
      const Result &input_result = this->get_input(input_socket->identifier);
      node_group_operation_result.share_data(input_result);
    }
  }
};

NodeOperation *get_group_output_node_operation(Context &context,
                                               const bNode &node,
                                               NodeGroupOperation &node_group_operation)
{
  return new GroupOutputNodeOperation(context, node, node_group_operation);
}

}  // namespace blender::compositor
