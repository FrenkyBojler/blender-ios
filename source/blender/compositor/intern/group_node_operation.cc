/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <memory>

#include "BLI_assert.h"
#include "BLI_vector.hh"

#include "BKE_node.hh"

#include "COM_group_node_operation.hh"
#include "COM_node_group_operation.hh"
#include "COM_node_operation.hh"
#include "COM_result.hh"

namespace blender::compositor {

class GroupNodeOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const bNodeTree *node_group = this->get_node_group();
    if (!node_group) {
      this->execute_invalid();
      return;
    }

    NodeGroupOperation node_group_operation(
        this->context(), *node_group, this->get_node_previews(), this->get_instance_key());

    Vector<std::unique_ptr<Result>> inputs;
    node_group->ensure_interface_cache();
    for (const bNodeTreeInterfaceSocket *input : node_group->interface_inputs()) {
      const Result &node_input_result = this->get_input(input->identifier);
      Result node_group_input_result = this->context().create_result(
          node_input_result.type(), node_input_result.precision());
      node_group_input_result.wrap_external(node_input_result);
      inputs.append(std::make_unique<Result>(node_group_input_result));
      node_group_operation.map_input_to_result(input->identifier, inputs.last().get());
    }

    node_group_operation.evaluate();

    for (const bNodeTreeInterfaceSocket *output : node_group->interface_outputs()) {
      Result &node_group_result = node_group_operation.get_result(output->identifier);
      Result &group_node_result = this->get_result(output->identifier);
      if (group_node_result.should_compute()) {
        group_node_result.share_data(node_group_result);
      }
      node_group_result.release();
    }
  }

  void execute_invalid()
  {
    const bNodeTree *node_group = this->get_node_group();
    for (const bNodeTreeInterfaceSocket *output : node_group->interface_outputs()) {
      Result &group_node_result = this->get_result(output->identifier);
      if (group_node_result.should_compute()) {
        group_node_result.allocate_invalid();
      }
    }
  }

  const bNodeTree *get_node_group()
  {
    BLI_assert(this->node().is_group());
    return reinterpret_cast<const bNodeTree *>(this->node().id);
  }
};

NodeOperation *get_group_node_operation(Context &context, const bNode &node)
{
  return new GroupNodeOperation(context, node);
}

}  // namespace blender::compositor
