/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_node_types.h"

#include "BKE_node.hh"

#include "COM_compile_state.hh"
#include "COM_context.hh"
#include "COM_group_node_operation.hh"
#include "COM_implicit_input_operation.hh"
#include "COM_input_descriptor.hh"
#include "COM_input_single_value_operation.hh"
#include "COM_multi_function_procedure_operation.hh"
#include "COM_node_group_operation.hh"
#include "COM_node_operation.hh"
#include "COM_operation.hh"
#include "COM_result.hh"
#include "COM_scheduler.hh"
#include "COM_shader_operation.hh"
#include "COM_undefined_node_operation.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

NodeGroupOperation::NodeGroupOperation(Context &context,
                                       const bNodeTree &node_group,
                                       const NodeGroupOutputTypes needed_outputs,
                                       Map<bNodeInstanceKey, bke::bNodePreview> *node_previews,
                                       const bNodeInstanceKey active_node_group_instance_key,
                                       const bNodeInstanceKey instance_key)
    : Operation(context),
      node_group_(node_group),
      needed_outputs_(needed_outputs),
      node_previews_(node_previews),
      active_node_group_instance_key_(active_node_group_instance_key),
      instance_key_(instance_key)
{
  node_group.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *input : node_group.interface_inputs()) {
    const InputDescriptor input_descriptor = input_descriptor_from_interface_input(node_group,
                                                                                   *input);
    this->declare_input_descriptor(input->identifier, input_descriptor);
  }

  /* The outputs connected to the group output node are not needed, so no need to declare results
   * for them.  */
  if (!flag_is_set(needed_outputs_, NodeGroupOutputTypes::GroupOutputNode)) {
    return;
  }

  for (const bNodeTreeInterfaceSocket *output : node_group.interface_outputs()) {
    const ResultType result_type = get_node_interface_socket_result_type(*output);
    this->populate_result(output->identifier, context.create_result(result_type));
  }
}

/* Checks if the node group with the given instance key has an active viewer node in it or in one
 * of its descendants. Only nodes of node groups whose instance key match that of the given active
 * viewer instance key are considered active. */
static bool has_active_viewer_node(const bNodeTree &node_group,
                                   const bNodeInstanceKey instance_key,
                                   const bNodeInstanceKey active_node_group_instance_key)
{
  /* This node group is not an being viewed by the user, so it has no active viewer regardless of
   * the existence of viewer nodes. */
  if (active_node_group_instance_key != instance_key) {
    return false;
  }

  /* An active viewer node exist, so return true. */
  for (const bNode *node : node_group.nodes_by_type("CompositorNodeViewer")) {
    if (node->flag & NODE_DO_OUTPUT && !node->is_muted()) {
      return true;
    }
  }

  /* For each of the group nodes, compute their instance key and call this function recursively. */
  for (const bNode *group_node : node_group.group_nodes()) {
    if (!group_node->id) {
      continue;
    }

    const bNodeTree &child_node_group = *reinterpret_cast<const bNodeTree *>(group_node->id);
    const bNodeInstanceKey child_instance_key = bke::node_instance_key(
        instance_key, &node_group, group_node);
    const bool active_viewer_node_found = has_active_viewer_node(
        child_node_group, child_instance_key, active_node_group_instance_key);

    /* Neither the child node group nor one of its descendant node groups has an active viewer
     * node, so we check other group nodes. */
    if (!active_viewer_node_found) {
      continue;
    }

    /* Otherwise, we have found our active context, return it. */
    return true;
  }

  /* Neither the child node group nor one of its descendant node groups has an active viewer node,
   * so return false. */
  return false;
}

/* Computes the outputs that are needed by the given particular node group with the given node
 * instance key, assuming that the currently active node group has the given instance key. This is
 * the same as the needed outputs supplied to the operation, except for viewer nodes and node
 * previews. Those are only computed for currently active node groups. An exception for viewer
 * nodes in root node groups exist, where if no viewer node exist in the possibly descendant active
 * node group, the viewer node in the root node group will be computed as a fallback. */
static NodeGroupOutputTypes compute_node_group_needed_outputs(
    const bNodeTree &node_group,
    const NodeGroupOutputTypes needed_outputs,
    const bNodeInstanceKey instance_key,
    const bNodeInstanceKey active_node_group_instance_key)
{
  /* Neither the viewer node or node previews are needed, so nothing needs to change. */
  if (!flag_is_set(needed_outputs, NodeGroupOutputTypes::ViewerNode) &&
      !flag_is_set(needed_outputs, NodeGroupOutputTypes::NodePreviews))
  {
    return needed_outputs;
  }

  /* If this is the active node group, then we need to compute the viewer node and node previews as
   * requested. */
  if (active_node_group_instance_key == instance_key) {
    return needed_outputs;
  }

  /* If no viewer node exist in the possibly descendant active node group and this is a root node
   * group, we fallback to the viewer in the root node group, but we don't need node previews. */
  if (!has_active_viewer_node(node_group, instance_key, active_node_group_instance_key) &&
      instance_key == bke::NODE_INSTANCE_KEY_BASE)
  {
    return needed_outputs & ~NodeGroupOutputTypes::NodePreviews;
  }

  /* This node group is not active, so no need to compute viewer node or previews. */
  return needed_outputs & ~(NodeGroupOutputTypes::ViewerNode | NodeGroupOutputTypes::NodePreviews);
}

void NodeGroupOperation::execute()
{
  const NodeGroupOutputTypes node_group_needed_outputs = compute_node_group_needed_outputs(
      node_group_, needed_outputs_, instance_key_, active_node_group_instance_key_);
  const VectorSet<const bNode *> schedule = compute_schedule(
      this->context(), node_group_, node_group_needed_outputs);
  CompileState compile_state(this->context(), schedule);

  for (const bNode *node : schedule) {
    if (this->context().is_canceled()) {
      this->cancel_evaluation();
      return;
    }

    if (compile_state.should_compile_pixel_compile_unit(*node)) {
      this->evaluate_pixel_compile_unit(compile_state);
    }

    if (is_pixel_node(*node)) {
      compile_state.add_node_to_pixel_compile_unit(*node);
    }
    else {
      this->evaluate_node(*node, compile_state);
    }
  }

  this->write_outputs(compile_state);
}

void NodeGroupOperation::write_outputs(CompileState &compile_state)
{
  if (!flag_is_set(needed_outputs_, NodeGroupOutputTypes::GroupOutputNode)) {
    return;
  }

  const bNode *group_output_node = node_group_.group_output_node();
  if (!group_output_node) {
    return;
  }

  for (const bNodeSocket *input : group_output_node->input_sockets()) {
    if (!is_socket_available(input)) {
      continue;
    }

    Result &output_result = this->get_result(input->identifier);
    if (!output_result.should_compute()) {
      continue;
    }

    const bNodeSocket *linked_output = get_output_linked_to_input(*input);
    /* If the input is linked, get the input result from the linked output, if not, get an input
     * single value result for it. */
    Result *input_result = linked_output ?
                               &compile_state.get_result_from_output_socket(*linked_output) :
                               &this->evaluate_input_single_value_operation(*input);

    /* So share the data of the result we get from the output with the result of the operation. */
    output_result.share_data(*input_result);

    /* Node operations typically call release of the results after execution, but the group
     * output node is an implicit node that doesn't have a corresponding node operation, so we
     * need to release the result here. */
    input_result->release();
  }
}

void NodeGroupOperation::evaluate_node(const bNode &node, CompileState &compile_state)
{
  /* Group input and group output nodes are implicit nodes and do not have corresponding
   * operations. The group output node is handled in the write_outputs method, while the group
   * input node is handled in the map_operation_input_to_group_input method. */
  if (node.is_group_input() || node.is_group_output()) {
    return;
  }

  NodeOperation *operation = this->get_node_operation(node);
  operation->set_instance_key(bke::node_instance_key(instance_key_, &node_group_, &node));

  /* Only set previews if the node group is currently being viewed. Except of the node is a group
   * node, because a child node group might currently be viewed. */
  if (node.is_group() || instance_key_ == active_node_group_instance_key_) {
    operation->set_node_previews(node_previews_);
  }

  compile_state.map_node_to_node_operation(node, operation);

  map_node_operation_inputs_to_their_results(node, operation, compile_state);

  /* This has to be done after input mapping because the method may add Input Single Value
   * Operations to the operations stream, which needs to be evaluated before the operation itself
   * is evaluated. */
  operations_stream_.append(std::unique_ptr<Operation>(operation));

  operation->compute_results_reference_counts(compile_state.get_schedule());

  operation->evaluate();
}

NodeOperation *NodeGroupOperation::get_node_operation(const bNode &node)
{
  const char *disabled_hint = nullptr;
  if (!node.typeinfo->poll(node.typeinfo, &node.owner_tree(), &disabled_hint)) {
    return get_undefined_node_operation(this->context(), node);
  }

  if (node.is_group()) {
    /* Make sure the GroupOutputNode output is always enabled for node group operations used by
     * group nodes. */
    return get_group_node_operation(this->context(),
                                    node,
                                    needed_outputs_ | NodeGroupOutputTypes::GroupOutputNode,
                                    active_node_group_instance_key_);
  }

  return node.typeinfo->get_compositor_operation(this->context(), node);
}

void NodeGroupOperation::map_node_operation_inputs_to_their_results(const bNode &node,
                                                                    NodeOperation *operation,
                                                                    CompileState &compile_state)
{
  for (const bNodeSocket *input : node.input_sockets()) {
    if (!is_socket_available(input)) {
      continue;
    }

    const bNodeSocket *output = get_output_linked_to_input(*input);
    if (output) {
      /* The input is linked to a group input node, which is a special case since the result comes
       * from the node group operation input itself. */
      if (output->owner_node().is_group_input()) {
        this->map_operation_input_to_group_input(*operation, input->identifier, *output);
        continue;
      }

      /* The input is linked. So map the input to the result we get from the output. */
      Result &result = compile_state.get_result_from_output_socket(*output);
      operation->map_input_to_result(input->identifier, &result);
      continue;
    }

    /* Otherwise, the input is unlinked. So map the input to the result of a newly created Input
     * Single Value Operation. */
    Result *input_single_value_result = &this->evaluate_input_single_value_operation(*input);
    operation->map_input_to_result(input->identifier, input_single_value_result);
  }
}

Result &NodeGroupOperation::evaluate_input_single_value_operation(const bNodeSocket &input)
{
  BLI_assert(!input.is_logically_linked());

  InputSingleValueOperation *input_operation = new InputSingleValueOperation(this->context(),
                                                                             input);
  operations_stream_.append(std::unique_ptr<InputSingleValueOperation>(input_operation));
  input_operation->evaluate();
  return input_operation->get_result();
}

/* Create one of the concrete subclasses of the PixelOperation based on the context and compile
 * state. Deleting the operation is the caller's responsibility. */
static PixelOperation *create_pixel_operation(Context &context, CompileState &compile_state)
{
  const VectorSet<const bNode *> &schedule = compile_state.get_schedule();
  PixelCompileUnit &compile_unit = compile_state.get_pixel_compile_unit();

  /* Use multi-function procedure to execute the pixel compile unit for CPU contexts or if the
   * compile unit is single value and would thus be more efficient to execute on the CPU. */
  if (!context.use_gpu() || compile_state.is_pixel_compile_unit_single_value()) {
    return new MultiFunctionProcedureOperation(context, compile_unit, schedule);
  }

  return new ShaderOperation(context, compile_unit, schedule);
}

void NodeGroupOperation::evaluate_pixel_compile_unit(CompileState &compile_state)
{
  PixelCompileUnit &compile_unit = compile_state.get_pixel_compile_unit();

  /* Pixel operations might have limitations on the number of outputs they can have, so we might
   * have to split the compile unit into smaller units to workaround this limitation. In practice,
   * splitting will almost always never happen due to the scheduling strategy we use, so the base
   * case remains fast. */
  int number_of_outputs = 0;
  for (int i : compile_unit.index_range()) {
    number_of_outputs += compile_state.compute_pixel_node_operation_outputs_count(
        *compile_unit[i], instance_key_ == active_node_group_instance_key_);

    if (number_of_outputs <= PixelOperation::maximum_number_of_outputs(this->context())) {
      continue;
    }

    /* The number of outputs surpassed the limit, so we split the compile unit into two equal parts
     * and recursively call this method on each of them. It might seem unexpected that we split in
     * half as opposed to split at the node that surpassed the limit, but that is because the act
     * of splitting might actually introduce new outputs, since links that were previously internal
     * to the compile unit might now be external. So we can't precisely split and guarantee correct
     * units, and we just rely or recursive splitting until units are small enough. Further, half
     * splitting helps balancing the shaders, where we don't want to have one gigantic shader and
     * a tiny one. */
    const int split_index = compile_unit.size() / 2;
    const PixelCompileUnit start_compile_unit(compile_unit.as_span().take_front(split_index));
    const PixelCompileUnit end_compile_unit(compile_unit.as_span().drop_front(split_index));

    compile_state.get_pixel_compile_unit() = start_compile_unit;
    this->evaluate_pixel_compile_unit(compile_state);

    compile_state.get_pixel_compile_unit() = end_compile_unit;
    this->evaluate_pixel_compile_unit(compile_state);

    /* No need to continue, the above recursive calls will eventually exist the loop and do the
     * actual compilation. */
    return;
  }

  PixelOperation *operation = create_pixel_operation(this->context(), compile_state);
  operation->set_instance_key(instance_key_);

  /* Only compute previews if the node group is currently being viewed. */
  if (instance_key_ == active_node_group_instance_key_) {
    operation->set_node_previews(node_previews_);
  }

  for (const bNode *node : compile_unit) {
    compile_state.map_node_to_pixel_operation(*node, operation);
  }

  map_pixel_operation_inputs_to_their_results(operation, compile_state);

  operations_stream_.append(std::unique_ptr<Operation>(operation));

  operation->compute_results_reference_counts(compile_state.get_schedule());

  operation->evaluate();

  compile_state.reset_pixel_compile_unit();
}

void NodeGroupOperation::map_pixel_operation_inputs_to_their_results(PixelOperation *operation,
                                                                     CompileState &compile_state)
{
  for (const auto item : operation->get_inputs_to_linked_outputs_map().items()) {
    const bNodeSocket &output = *item.value;
    const StringRef input_identifier = item.key;

    /* The input is linked to a group input node, which is a special case since the result comes
     * from the node group operation input itself. */
    Result *input_result = nullptr;
    if (output.owner_node().is_group_input()) {
      input_result = &this->map_operation_input_to_group_input(
          *operation, input_identifier, output);
    }
    else {
      input_result = &compile_state.get_result_from_output_socket(output);
      operation->map_input_to_result(input_identifier, input_result);
    }

    /* Correct the reference count of the result in case multiple of the result's outgoing links
     * corresponds to a single input in the pixel operation. See the description of the member
     * inputs_to_reference_counts_map_ variable for more information. */
    const int internal_reference_count = operation->get_internal_input_reference_count(
        input_identifier);
    input_result->decrement_reference_count(internal_reference_count - 1);
  }

  for (const auto item : operation->get_implicit_inputs_to_input_identifiers_map().items()) {
    ImplicitInputOperation *input_operation = new ImplicitInputOperation(this->context(),
                                                                         item.key);
    operation->map_input_to_result(item.value, &input_operation->get_result());

    operations_stream_.append(std::unique_ptr<ImplicitInputOperation>(input_operation));

    input_operation->evaluate();
  }
}

Result &NodeGroupOperation::map_operation_input_to_group_input(Operation &operation,
                                                               const StringRef input_identifier,
                                                               const bNodeSocket &output)
{
  BLI_assert(output.owner_node().is_group_input());

  Result &input_result = this->get_input(output.identifier);
  operation.map_input_to_result(input_identifier, &input_result);
  input_result.increment_reference_count();

  return input_result;
}

void NodeGroupOperation::cancel_evaluation()
{
  for (const std::unique_ptr<Operation> &operation : operations_stream_) {
    operation->free_results();
  }
}

}  // namespace blender::compositor
