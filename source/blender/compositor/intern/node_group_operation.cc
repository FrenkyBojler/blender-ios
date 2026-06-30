/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_compute_context.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "DNA_node_types.h"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_zones.hh"

#include "NOD_eval_log.hh"

#include "COM_compile_state.hh"
#include "COM_context.hh"
#include "COM_group_input_node_operation.hh"
#include "COM_group_node_operation.hh"
#include "COM_group_output_node_operation.hh"
#include "COM_implicit_input_operation.hh"
#include "COM_input_descriptor.hh"
#include "COM_multi_function_procedure_operation.hh"
#include "COM_node_group_operation.hh"
#include "COM_node_operation.hh"
#include "COM_operation.hh"
#include "COM_result.hh"
#include "COM_scheduler.hh"
#include "COM_shader_operation.hh"
#include "COM_single_value_node_input_operation.hh"
#include "COM_undefined_node_operation.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

NodeGroupOperation::NodeGroupOperation(Context &context,
                                       const bNodeTree &node_group,
                                       const NodeGroupOutputTypes needed_outputs,
                                       const bNodeInstanceKey active_node_group_instance_key,
                                       const bNodeInstanceKey instance_key,
                                       const ComputeContext &compute_context)
    : Operation(context),
      node_group_(node_group),
      needed_output_types_(needed_outputs),
      active_node_group_instance_key_(active_node_group_instance_key),
      instance_key_(instance_key),
      compute_context_(compute_context)
{
  node_group.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *input : node_group.interface_inputs()) {
    const InputDescriptor input_descriptor = input_descriptor_from_interface_input(node_group,
                                                                                   *input);
    this->declare_input_descriptor(input->identifier, input_descriptor);
  }

  for (const bNodeTreeInterfaceSocket *output : node_group.interface_outputs()) {
    this->populate_result(output->identifier, get_node_interface_socket_result_type(*output));
  }
}

class ScopedNodeGroupTimer {
 private:
  const ComputeContext &compute_context_;
  nodes::eval_log::NodesEvalLog *log_;

  nodes::eval_log::TimePoint start_;

 public:
  ScopedNodeGroupTimer(const ComputeContext &compute_context, nodes::eval_log::NodesEvalLog *log)
      : compute_context_(compute_context), log_(log)
  {
    start_ = nodes::eval_log::Clock::now();
  }

  ~ScopedNodeGroupTimer()
  {
    if (!log_) {
      return;
    }
    const nodes::eval_log::TimePoint end = nodes::eval_log::Clock::now();
    nodes::eval_log::NodeTreeLogger &tree_logger = log_->get_local_tree_logger(compute_context_);
    tree_logger.execution_time = end - start_;
  }
};

void NodeGroupOperation::execute()
{
  const ScopedNodeGroupTimer node_group_timer{compute_context_,
                                              this->context().nodes_evaluation_log()};
  const Schedule schedule = compute_schedule(this->context(),
                                             node_group_,
                                             *this,
                                             needed_output_types_,
                                             instance_key_,
                                             active_node_group_instance_key_);
  CompileState compile_state(this->context(), schedule);

  /* Nodes handled by evaluate_repeat_zone are skipped in the main loop. */
  Set<const bNode *> handled_nodes;

  for (const bNode *node : schedule.nodes) {
    if (this->context().is_canceled()) {
      this->cancel_evaluation();
      break;
    }

    /* Skip nodes already consumed by a repeat zone evaluation. */
    if (handled_nodes.contains(node)) {
      continue;
    }

    if (compile_state.should_compile_pixel_compile_unit(*node)) {
      this->evaluate_pixel_compile_unit(compile_state);
    }

    /* Intercept repeat zone input nodes and evaluate the full zone (N iterations).
     * The pixel compile unit flush above must happen first so that any pending pixel
     * nodes (e.g. an Image node before the zone) are compiled into a PixelOperation
     * and can be looked up when importing border-source results into the body state. */
    if (node->type_legacy == CMP_NODE_REPEAT_INPUT) {
      this->evaluate_repeat_zone(*node, compile_state, handled_nodes);
      continue;
    }

    if (is_pixel_node(*node)) {
      compile_state.add_node_to_pixel_compile_unit(*node);
    }
    else {
      this->evaluate_node(*node, compile_state);
    }
  }

  /* Some of the needed outputs might not be allocated even after execution. This could happen for
   * instance when no Group Output node exist or when the evaluation gets canceled before the
   * output is written. */
  this->allocate_default_remaining_outputs();
}

void NodeGroupOperation::evaluate_node(const bNode &node, CompileState &compile_state)
{
  NodeOperation *operation = this->get_node_operation(node);
  operation->set_instance_key(bke::node_instance_key(instance_key_, &node_group_, &node));
  operation->set_compute_context(compute_context_);

  /* Only compute previews if the node group is currently being viewed. */
  operation->set_needs_node_previews(
      bool(needed_output_types_ & NodeGroupOutputTypes::NodePreviews) &&
      instance_key_ == active_node_group_instance_key_);

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
    return get_group_node_operation(
        this->context(), node, needed_output_types_, active_node_group_instance_key_);
  }

  if (node.is_group_output()) {
    return get_group_output_node_operation(this->context(), node, *this);
  }

  if (node.is_group_input()) {
    return get_group_input_node_operation(this->context(), node, *this);
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
    if (output && compile_state.get_schedule().nodes.contains(&output->owner_node()) &&
        !compile_state.get_schedule().unneeded_inputs.contains(input))
    {
      /* The input is linked to a node that is part of the schedule. So map the input to the result
       * we get from the output. */
      Result &result = compile_state.get_result_from_output_socket(*output);
      operation->map_input_to_result(input->identifier, &result);
      continue;
    }

    const InputDescriptor input_descriptor = input_descriptor_from_input_socket(input);
    if (!input_descriptor.implicit_input.has_value()) {
      /* The input is unlinked with no implicit value. So map the input to the result of a newly
       * created Input Single Value Operation. */
      SingleValueNodeInputOperation *input_operation = new SingleValueNodeInputOperation(
          this->context(), *input);
      operations_stream_.append(std::unique_ptr<SingleValueNodeInputOperation>(input_operation));
      input_operation->evaluate();
      operation->map_input_to_result(input->identifier, &input_operation->get_result());
      continue;
    }

    ImplicitInputOperation *input_operation = new ImplicitInputOperation(
        this->context(), input_descriptor.implicit_input.value());
    operations_stream_.append(std::unique_ptr<ImplicitInputOperation>(input_operation));
    input_operation->evaluate();
    operation->map_input_to_result(input->identifier, &input_operation->get_result());
  }
}

/* Create one of the concrete subclasses of the PixelOperation based on the context and compile
 * state. Deleting the operation is the caller's responsibility. */
static PixelOperation *create_pixel_operation(Context &context,
                                              CompileState &compile_state,
                                              const ComputeContext &compute_context)
{
  const Schedule &schedule = compile_state.get_schedule();
  PixelCompileUnit &compile_unit = compile_state.get_pixel_compile_unit();

  /* Use multi-function procedure to execute the pixel compile unit for CPU contexts or if the
   * compile unit is single value and would thus be more efficient to execute on the CPU. */
  const bool is_single_value = compile_state.is_pixel_compile_unit_single_value();
  if (!context.use_gpu() || is_single_value) {
    return new MultiFunctionProcedureOperation(
        context, compile_unit, schedule, is_single_value, compute_context);
  }

  return new ShaderOperation(context, compile_unit, schedule, compute_context);
}

void NodeGroupOperation::evaluate_pixel_compile_unit(CompileState &compile_state)
{
  PixelCompileUnit &compile_unit = compile_state.get_pixel_compile_unit();

  /* Pixel operations might have limitations on the number of outputs or inputs they can have, so
   * we might have to split the compile unit into smaller units to workaround this limitation. In
   * practice, splitting will almost always never happen due to the scheduling strategy we use, so
   * the base case remains fast. */
  const bool are_node_previews_needed = instance_key_ == active_node_group_instance_key_;
  if (compile_state.pixel_compile_unit_has_too_many_outputs(are_node_previews_needed) ||
      compile_state.pixel_compile_unit_has_too_many_inputs())
  {
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

  PixelOperation *operation = create_pixel_operation(
      this->context(), compile_state, compute_context_);

  /* Only compute previews if the node group is currently being viewed. */
  operation->set_needs_node_previews(
      bool(needed_output_types_ & NodeGroupOutputTypes::NodePreviews) &&
      instance_key_ == active_node_group_instance_key_);

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

    Result *input_result = compile_state.try_get_result_from_output_socket(output);
    if (!input_result) {
      /* The source node is not in the compile state (e.g., a body pixel node whose input links
       * to a node that was not yet evaluated). Find the consumer input socket in the compile unit
       * and substitute the socket's default value. */
      const bNodeSocket *consumer = nullptr;
      for (const bNode *node : compile_state.get_pixel_compile_unit()) {
        for (const bNodeSocket *in : node->input_sockets()) {
          if (get_output_linked_to_input(*in) == &output) {
            consumer = in;
            break;
          }
        }
        if (consumer) {
          break;
        }
      }
      if (!consumer) {
        continue;
      }
      SingleValueNodeInputOperation *sv_op = new SingleValueNodeInputOperation(
          this->context(), *consumer);
      operations_stream_.append(std::unique_ptr<SingleValueNodeInputOperation>(sv_op));
      sv_op->evaluate();
      input_result = &sv_op->get_result();
    }

    operation->map_input_to_result(input_identifier, input_result);

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

void NodeGroupOperation::evaluate_repeat_zone(const bNode &zone_input_node,
                                               CompileState &compile_state,
                                               Set<const bNode *> &handled_nodes)
{
  const Schedule &schedule = compile_state.get_schedule();
  const bNodeTree &tree = node_group_;

  const bke::bNodeTreeZones *zones = tree.zones();
  if (!zones) {
    this->evaluate_node(zone_input_node, compile_state);
    return;
  }

  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(zone_input_node.identifier);
  if (!zone || !zone->output_node_id.has_value()) {
    this->evaluate_node(zone_input_node, compile_state);
    return;
  }

  const bNode *zone_output_node = tree.node_by_id(*zone->output_node_id);
  if (!zone_output_node) {
    this->evaluate_node(zone_input_node, compile_state);
    return;
  }

  /* Mark all zone body nodes and the zone output node as handled so the main loop skips them. */
  for (const int node_id : zone->child_node_ids) {
    if (const bNode *child = tree.node_by_id(node_id)) {
      handled_nodes.add(child);
    }
  }
  handled_nodes.add(zone_output_node);

  /* ----------------------------------------------------------------
   * Read the iteration count from the "Iterations" socket.
   * ---------------------------------------------------------------- */
  int iterations = 1;
  for (const bNodeSocket *input : zone_input_node.input_sockets()) {
    if (StringRef(input->identifier) != "Iterations") {
      continue;
    }
    const bNodeSocket *linked_output = get_output_linked_to_input(*input);
    if (linked_output && schedule.nodes.contains(&linked_output->owner_node())) {
      Result &result = compile_state.get_result_from_output_socket(*linked_output);
      /* Guard against non-numeric types being connected to the Iterations socket. */
      switch (result.type()) {
        case ResultType::Int:
          iterations = result.get_single_value<int>();
          break;
        case ResultType::Float:
          iterations = int(result.get_single_value<float>());
          break;
        case ResultType::Bool:
          iterations = int(result.get_single_value<bool>());
          break;
        default:
          iterations = input->default_value_typed<bNodeSocketValueInt>()->value;
          break;
      }
      /* This result was counted as consumed by zone_input in the main schedule. Since we handle
       * zone_input ourselves, release it here to properly decrement its reference count. */
      result.release();
    }
    else {
      /* Unlinked — fall back to the socket's default value. */
      iterations = input->default_value_typed<bNodeSocketValueInt>()->value;
    }
    break;
  }
  iterations = std::max(0, iterations);

  /* ----------------------------------------------------------------
   * Collect initial carry values for all sockets that thread through
   * iterations: "Image" plus any user-added items (by identifier).
   * These are all available inputs on zone_output_node except
   * "__extend__", which we mirror from zone_input_node's inputs.
   * ---------------------------------------------------------------- */
  Map<StringRef, Result *> current_values;
  for (const bNodeSocket *out_input : zone_output_node->input_sockets()) {
    if (!is_socket_available(out_input)) {
      continue;
    }
    if (StringRef(out_input->identifier) == "__extend__") {
      continue;
    }
    const StringRef id = out_input->identifier;
    /* Find the matching input on zone_input_node to get the initial value. */
    for (const bNodeSocket *in_input : zone_input_node.input_sockets()) {
      if (StringRef(in_input->identifier) != id) {
        continue;
      }
      const bNodeSocket *linked_output = get_output_linked_to_input(*in_input);
      if (linked_output && schedule.nodes.contains(&linked_output->owner_node())) {
        current_values.add(id, &compile_state.get_result_from_output_socket(*linked_output));
      }
      break;
    }
    /* If no linked source, leave out of map — SingleValue fallback used per-iteration. */
  }

  /* ----------------------------------------------------------------
   * Build body_node_ids and border_source_nodes early — both are
   * needed for the early-evaluation pass and refcount adjustment.
   * ---------------------------------------------------------------- */
  Set<int32_t> body_node_ids;
  for (const int id : zone->child_node_ids) {
    body_node_ids.add(id);
  }

  Set<const bNode *> border_source_nodes;
  for (const bNodeLink *link : zone->border_links) {
    if (!link || !link->fromsock) {
      continue;
    }
    const bNode &from_node = link->fromsock->owner_node();
    if (schedule.nodes.contains(&from_node)) {
      border_source_nodes.add(&from_node);
    }
  }

  /* ----------------------------------------------------------------
   * Evaluate border-source nodes (and their transitive dependencies)
   * that the main execute() loop hasn't reached yet.
   *
   * Due to DFS topological ordering, border sources may appear AFTER
   * zone_input in the main schedule when they are reachable only
   * through zone body nodes. We must compile them here so their
   * results exist before the refcount adjustment and body iterations.
   * Mark them in handled_nodes so the main loop skips them.
   * ---------------------------------------------------------------- */
  {
    /* BFS from border sources to collect all transitive non-body deps. */
    Set<const bNode *> nodes_to_evaluate;
    Vector<const bNode *> worklist;
    for (const bNode *n : border_source_nodes) {
      if (nodes_to_evaluate.add(n)) {
        worklist.append(n);
      }
    }
    for (int i = 0; i < int(worklist.size()); i++) {
      const bNode *n = worklist[i];
      for (const bNodeSocket *input : n->input_sockets()) {
        const bNodeSocket *linked_output = get_output_linked_to_input(*input);
        if (!linked_output) {
          continue;
        }
        const bNode &dep = linked_output->owner_node();
        if (body_node_ids.contains(dep.identifier) || &dep == &zone_input_node) {
          continue;
        }
        if (!schedule.nodes.contains(&dep)) {
          continue;
        }
        if (nodes_to_evaluate.add(&dep)) {
          worklist.append(&dep);
        }
      }
    }

    /* Evaluate in topological order (main schedule order is correct). */
    for (const bNode *n : schedule.nodes) {
      if (!nodes_to_evaluate.contains(n)) {
        continue;
      }
      if (compile_state.get_node_operation(*n) || compile_state.get_pixel_operation(*n)) {
        continue;  /* Already compiled by the main loop. */
      }
      if (compile_state.should_compile_pixel_compile_unit(*n)) {
        this->evaluate_pixel_compile_unit(compile_state);
      }
      if (is_pixel_node(*n)) {
        compile_state.add_node_to_pixel_compile_unit(*n);
      }
      else {
        this->evaluate_node(*n, compile_state);
      }
      handled_nodes.add(n);
    }
    if (!compile_state.get_pixel_compile_unit().is_empty()) {
      this->evaluate_pixel_compile_unit(compile_state);
    }
  }

  /* ----------------------------------------------------------------
   * Multiply the reference counts of border-link results so they
   * survive being consumed once per iteration.
   *
   * border_links: links from nodes OUTSIDE the zone into zone body
   * nodes. For N iterations each such source is used K×N times
   * total (K = number of zone body consumers in the main schedule).
   * The main schedule already set ref_count = K, so add K×(N-1).
   * ---------------------------------------------------------------- */
  if (iterations > 1) {
    /* Count how many border links share each fromsock. */
    Map<const bNodeSocket *, int> fromsock_use_counts;
    for (const bNodeLink *link : zone->border_links) {
      if (!link || !link->fromsock) {
        continue;
      }
      const bNode &from_node = link->fromsock->owner_node();
      if (!schedule.nodes.contains(&from_node)) {
        continue;
      }
      fromsock_use_counts.lookup_or_add(link->fromsock, 0)++;
    }
    for (const auto &item : fromsock_use_counts.items()) {
      Result &result = compile_state.get_result_from_output_socket(*item.key);
      result.set_reference_count(result.reference_count() + item.value * (iterations - 1));
    }
  }

  Schedule body_schedule;
  /* External nodes first. */
  for (const bNode *n : schedule.nodes) {
    if (border_source_nodes.contains(n)) {
      body_schedule.nodes.add_new(n);
    }
  }
  /* Zone input. */
  body_schedule.nodes.add_new(&zone_input_node);
  /* Body nodes in topo order. */
  for (const bNode *n : schedule.nodes) {
    if (body_node_ids.contains(n->identifier)) {
      body_schedule.nodes.add_new(n);
    }
  }
  /* Zone output (for ref-count purposes only; not actually evaluated in the body loop). */
  body_schedule.nodes.add_new(zone_output_node);

  /* Propagate unneeded-input markers that apply to zone body nodes. */
  for (const bNodeSocket *sock : schedule.unneeded_inputs) {
    const bNode &owner = sock->owner_node();
    if (&owner == &zone_input_node || body_node_ids.contains(owner.identifier) ||
        &owner == zone_output_node)
    {
      body_schedule.unneeded_inputs.add(sock);
    }
  }

  /* ----------------------------------------------------------------
   * Run the zone body N times.
   * current_values carries all carry-through sockets; each iteration
   * overwrites them with that iteration's outputs.
   * ---------------------------------------------------------------- */
  for (int iteration = 0; iteration < iterations; iteration++) {
    if (this->context().is_canceled()) {
      break;
    }

    CompileState body_state(this->context(), body_schedule);

    /* Import ALL external nodes linked to any body node input into body_state.
     * This is more robust than limiting to zone->border_links, which may miss
     * nodes depending on zone topology. zone_input_node is excluded here and
     * mapped separately below after its operation is created. */
    for (const bNode *body_node : schedule.nodes) {
      if (!body_node_ids.contains(body_node->identifier)) {
        continue;
      }
      for (const bNodeSocket *input : body_node->input_sockets()) {
        const bNodeSocket *linked_output = get_output_linked_to_input(*input);
        if (!linked_output) {
          continue;
        }
        const bNode &from_node = linked_output->owner_node();
        if (body_node_ids.contains(from_node.identifier) || &from_node == &zone_input_node) {
          continue;
        }
        if (NodeOperation *op = compile_state.get_node_operation(from_node)) {
          if (!body_state.get_node_operation(from_node)) {
            body_state.map_node_to_node_operation(from_node, op);
          }
        }
        else if (PixelOperation *pix = compile_state.get_pixel_operation(from_node)) {
          if (!body_state.get_pixel_operation(from_node)) {
            body_state.map_node_to_pixel_operation(from_node, pix);
          }
        }
      }
    }

    /* Create a fresh RepeatInputOperation for this iteration and map its "Image" input. */
    NodeOperation *zone_input_op =
        zone_input_node.typeinfo->get_compositor_operation(this->context(), zone_input_node);
    zone_input_op->set_instance_key(
        bke::node_instance_key(instance_key_, &node_group_, &zone_input_node));
    zone_input_op->set_compute_context(compute_context_);
    zone_input_op->set_needs_node_previews(false);

    /* Map all available zone_input_op inputs. Carry sockets (Image + items) get
     * the current accumulated value, or a single-value default if not yet set.
     * Other inputs (e.g. Iterations) get single-value defaults. */
    for (const bNodeSocket *input : zone_input_node.input_sockets()) {
      if (!is_socket_available(input)) {
        continue;
      }
      if (StringRef(input->identifier) == "__extend__") {
        continue;
      }
      const InputDescriptor desc = input_descriptor_from_input_socket(input);
      if (desc.implicit_input.has_value()) {
        continue;
      }
      Result **carry = current_values.lookup_ptr(input->identifier);
      if (carry && *carry) {
        zone_input_op->map_input_to_result(input->identifier, *carry);
      }
      else {
        /* Unlinked or not a carry socket — use the socket's default value. */
        SingleValueNodeInputOperation *sv_op = new SingleValueNodeInputOperation(
            this->context(), *input);
        operations_stream_.append(std::unique_ptr<SingleValueNodeInputOperation>(sv_op));
        sv_op->evaluate();
        zone_input_op->map_input_to_result(input->identifier, &sv_op->get_result());
      }
    }

    body_state.map_node_to_node_operation(zone_input_node, zone_input_op);
    operations_stream_.append(std::unique_ptr<Operation>(zone_input_op));
    zone_input_op->compute_results_reference_counts(body_schedule);
    zone_input_op->evaluate();

    /* Fix the Iteration output: execute() always writes 0; overwrite with the actual index. */
    if (Result &iter_result = zone_input_op->get_result("Iteration"); iter_result.is_allocated()) {
      iter_result.set_single_value(iteration);
    }

    /* Evaluate body nodes in topological order (mirroring the main execute() loop). */
    for (const bNode *body_node : schedule.nodes) {
      if (!body_node_ids.contains(body_node->identifier)) {
        continue;
      }

      if (body_state.should_compile_pixel_compile_unit(*body_node)) {
        this->evaluate_pixel_compile_unit(body_state);
      }

      if (is_pixel_node(*body_node)) {
        body_state.add_node_to_pixel_compile_unit(*body_node);
      }
      else {
        this->evaluate_node(*body_node, body_state);
      }
    }

    /* Flush any remaining pixel compile unit. */
    if (!body_state.get_pixel_compile_unit().is_empty()) {
      this->evaluate_pixel_compile_unit(body_state);
    }

    /* Extract carry values from the sockets linked to zone_output's inputs. */
    current_values.clear();
    for (const bNodeSocket *input : zone_output_node->input_sockets()) {
      if (!is_socket_available(input)) {
        continue;
      }
      if (StringRef(input->identifier) == "__extend__") {
        continue;
      }
      const bNodeSocket *linked = get_output_linked_to_input(*input);
      if (linked) {
        current_values.add(input->identifier,
                           &body_state.get_result_from_output_socket(*linked));
      }
    }

    /* If no carry sockets are linked, stop iterating — nothing to feed back. */
    if (current_values.is_empty()) {
      break;
    }
  }

  /* ----------------------------------------------------------------
   * Evaluate the zone output node once with the final image.
   * This registers its output in the main compile_state so
   * downstream nodes can find it via get_result_from_output_socket.
   * ---------------------------------------------------------------- */
  NodeOperation *zone_output_op =
      zone_output_node->typeinfo->get_compositor_operation(this->context(), *zone_output_node);
  zone_output_op->set_instance_key(
      bke::node_instance_key(instance_key_, &node_group_, zone_output_node));
  zone_output_op->set_compute_context(compute_context_);
  zone_output_op->set_needs_node_previews(false);

  compile_state.map_node_to_node_operation(*zone_output_node, zone_output_op);

  /* Map all carry inputs on zone_output_op from final current_values,
   * falling back to the socket default for any unlinked or unset carry socket. */
  for (const bNodeSocket *input : zone_output_node->input_sockets()) {
    if (!is_socket_available(input)) {
      continue;
    }
    if (StringRef(input->identifier) == "__extend__") {
      continue;
    }
    Result **carry = current_values.lookup_ptr(input->identifier);
    if (carry && *carry) {
      zone_output_op->map_input_to_result(input->identifier, *carry);
    }
    else {
      SingleValueNodeInputOperation *sv_op = new SingleValueNodeInputOperation(
          this->context(), *input);
      operations_stream_.append(std::unique_ptr<SingleValueNodeInputOperation>(sv_op));
      sv_op->evaluate();
      zone_output_op->map_input_to_result(input->identifier, &sv_op->get_result());
    }
  }

  operations_stream_.append(std::unique_ptr<Operation>(zone_output_op));
  zone_output_op->compute_results_reference_counts(schedule);
  zone_output_op->evaluate();
}

void NodeGroupOperation::cancel_evaluation()
{
  for (const std::unique_ptr<Operation> &operation : operations_stream_) {
    operation->free_results();
  }
}

}  // namespace blender::compositor
