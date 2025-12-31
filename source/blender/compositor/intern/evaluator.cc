/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <memory>

#include "BLI_vector.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "COM_context.hh"
#include "COM_evaluator.hh"
#include "COM_node_group_operation.hh"
#include "COM_operation.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

class WriteOutputOperation : public Operation {
 public:
  constexpr static const StringRef input_identifier = StringRef("Input");

  WriteOutputOperation(Context &context) : Operation(context)
  {
    this->declare_input_descriptor(WriteOutputOperation::input_identifier,
                                   InputDescriptor{ResultType::Color});
  }

  void execute() override
  {
    this->context().write_output(this->get_input(WriteOutputOperation::input_identifier));
  }

  Domain compute_domain() override
  {
    if (this->context().use_compositing_domain_for_input_output()) {
      return this->context().get_compositing_domain();
    }
    return Operation::compute_domain();
  }
};

void evaluate(Context &context,
              const bNodeTree &node_group,
              const NodeGroupOutputTypes needed_outputs)
{
  Map<bNodeInstanceKey, bke::bNodePreview> *node_previews =
      flag_is_set(needed_outputs, NodeGroupOutputTypes::NodePreviews) ?
          &node_group.runtime->previews :
          nullptr;
  NodeGroupOperation node_group_operation(context,
                                          node_group,
                                          needed_outputs,
                                          node_previews,
                                          node_group.active_viewer_key,
                                          bke::NODE_INSTANCE_KEY_BASE);

  Vector<std::unique_ptr<Result>> inputs;
  node_group.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *input : node_group.interface_inputs()) {
    const Result input_result = context.get_input(input->identifier);
    if (input_result.is_allocated()) {
      inputs.append(std::make_unique<Result>(input_result));
    }
    else {
      const ResultType input_type = get_node_interface_socket_result_type(*input);
      Result invalid_result = context.create_result(input_type);
      invalid_result.allocate_invalid();
      inputs.append(std::make_unique<Result>(invalid_result));
    }

    node_group_operation.map_input_to_result(input->identifier, inputs.last().get());
  }

  node_group_operation.evaluate();

  if (node_group.interface_outputs().is_empty()) {
    return;
  }

  if (context.is_canceled()) {
    for (const bNodeTreeInterfaceSocket *output : node_group.interface_outputs()) {
      node_group_operation.get_result(output->identifier).release();
    }
    return;
  }

  const bNodeTreeInterfaceSocket *output_socket = node_group.interface_outputs()[0];
  Result &output_result = node_group_operation.get_result(output_socket->identifier);

  WriteOutputOperation write_output_operation(context);
  write_output_operation.map_input_to_result(WriteOutputOperation::input_identifier,
                                             &output_result);
  write_output_operation.evaluate();

  for (const bNodeTreeInterfaceSocket *output : node_group.interface_outputs().drop_front(1)) {
    node_group_operation.get_result(output->identifier).release();
  }
}

}  // namespace blender::compositor
