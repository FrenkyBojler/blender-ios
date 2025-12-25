/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <memory>

#include "BLI_vector.hh"

#include "COM_context.hh"
#include "COM_evaluator.hh"
#include "COM_node_group_operation.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

Evaluator::Evaluator(Context &context) : context_(context) {}

void Evaluator::evaluate()
{
  const bNodeTree &node_group = context_.get_node_tree();
  NodeGroupOperation node_group_operation(context_, node_group);

  Vector<std::unique_ptr<Result>> inputs;
  node_group.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *input : node_group.interface_inputs()) {
    const Result input_result = context_.get_input(input->identifier);
    if (input_result.is_allocated()) {
      inputs.append(std::make_unique<Result>(input_result));
    }
    else {
      const ResultType input_type = get_node_interface_socket_result_type(*input);
      Result invalid_result = context_.create_result(input_type);
      invalid_result.allocate_invalid();
      inputs.append(std::make_unique<Result>(invalid_result));
    }

    node_group_operation.map_input_to_result(input->identifier, inputs.last().get());
  }

  node_group_operation.evaluate();

  const bNodeTreeInterfaceSocket *output = node_group.interface_outputs()[0];
  if (StringRef(output->socket_type) != "NodeSocketColor") {
    return;
  }
  context_.write_output(node_group_operation.get_result(output->identifier));
}

}  // namespace blender::compositor
