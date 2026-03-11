/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_lazy_function.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_nodes_reference_set.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"

#include "FN_lazy_function_execute.hh"

#include "BLT_translation.hh"

#include "BLI_array_utils.hh"

#include "DEG_depsgraph_query.hh"

#include "FN_lazy_function_graph_executor.hh"

namespace blender::nodes {

using bke::SocketValueVariant;

/**
 * Wraps the execution of a repeat loop body. The purpose is to setup the correct #ComputeContext
 * inside of the loop body. This is necessary to support correct logging inside of a repeat zone.
 * An alternative would be to use a separate `LazyFunction` for every iteration, but that would
 * have higher overhead.
 */
class RepeatBodyNodeExecuteWrapper : public lf::GraphExecutorNodeExecuteWrapper {
 public:
  const bNode *repeat_output_bnode_ = nullptr;
  VectorSet<lf::FunctionNode *> *lf_body_nodes_ = nullptr;

  void execute_node(const lf::FunctionNode &node,
                    lf::Params &params,
                    const lf::Context &context) const override
  {
    GeoNodesUserData &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    const int iteration = lf_body_nodes_->index_of_try(const_cast<lf::FunctionNode *>(&node));
    const LazyFunction &fn = node.function();
    if (iteration == -1) {
      /* The node is not a loop body node, just execute it normally. */
      fn.execute(params, context);
      return;
    }

    /* Setup context for the loop body evaluation. */
    bke::RepeatZoneComputeContext body_compute_context{
        user_data.compute_context, *repeat_output_bnode_, iteration};
    GeoNodesUserData body_user_data = user_data;
    body_user_data.compute_context = &body_compute_context;
    body_user_data.log_socket_values = should_log_socket_values_for_context(
        user_data, body_compute_context.hash());

    GeoNodesLocalUserData body_local_user_data{body_user_data};
    lf::Context body_context{context.storage, &body_user_data, &body_local_user_data};
    fn.execute(params, body_context);
  }
};

/**
 * Knows which iterations of the loop evaluation have side effects.
 */
class RepeatZoneSideEffectProvider : public lf::GraphExecutorSideEffectProvider {
 public:
  const bNode *repeat_output_bnode_ = nullptr;
  Span<lf::FunctionNode *> lf_body_nodes_;

  Vector<const lf::FunctionNode *> get_nodes_with_side_effects(
      const lf::Context &context) const override
  {
    GeoNodesUserData &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    const GeoNodesCallData &call_data = *user_data.call_data;
    if (!call_data.side_effect_nodes) {
      return {};
    }
    const ComputeContextHash &context_hash = user_data.compute_context->hash();
    const Span<int> iterations_with_side_effects =
        call_data.side_effect_nodes->iterations_by_iteration_zone.lookup(
            {context_hash, repeat_output_bnode_->identifier});

    Vector<const lf::FunctionNode *> lf_nodes;
    for (const int i : iterations_with_side_effects) {
      if (i >= 0 && i < lf_body_nodes_.size()) {
        lf_nodes.append(lf_body_nodes_[i]);
      }
    }
    return lf_nodes;
  }
};

struct GenericEvalStorage {
  VectorSet<lf::FunctionNode *> lf_body_nodes;
  lf::Graph graph;
  std::optional<LazyFunctionForLogicalOr> or_function;
  std::optional<RepeatZoneSideEffectProvider> side_effect_provider;
  std::optional<RepeatBodyNodeExecuteWrapper> body_execute_wrapper;
  std::optional<lf::GraphExecutor> graph_executor;
  Array<SocketValueVariant> index_values;
  void *graph_executor_storage = nullptr;
  bool multi_threading_enabled = false;
  Vector<int> input_index_map;
  Vector<int> output_index_map;
};

struct RepeatEvalStorage {
  ResourceScope scope;
  bool checked_inspection_index = false;
  GenericEvalStorage *generic = nullptr;
};

class LazyFunctionForRepeatZone : public LazyFunction {
 private:
  const bNodeTree &btree_;
  const bke::bNodeTreeZone &zone_;
  const bNode &repeat_output_bnode_;
  const ZoneBuildInfo &zone_info_;
  const ZoneBodyFunction &body_fn_;

 public:
  LazyFunctionForRepeatZone(const bNodeTree &btree,
                            const bke::bNodeTreeZone &zone,
                            ZoneBuildInfo &zone_info,
                            const ZoneBodyFunction &body_fn)
      : btree_(btree),
        zone_(zone),
        repeat_output_bnode_(*zone.output_node()),
        zone_info_(zone_info),
        body_fn_(body_fn)
  {
    debug_name_ = "Repeat Zone";

    initialize_zone_wrapper(zone, zone_info, body_fn, true, inputs_, outputs_);
    /* Iterations input is always used. */
    inputs_[zone_info.indices.inputs.main[0]].usage = lf::ValueUsage::Used;
  }

  void *init_storage(LinearAllocator<> &allocator) const override
  {
    return allocator.construct<RepeatEvalStorage>().release();
  }

  void destruct_storage(void *storage) const override
  {
    RepeatEvalStorage *s = static_cast<RepeatEvalStorage *>(storage);
    if (s->generic && s->generic->graph_executor_storage) {
      s->generic->graph_executor->destruct_storage(s->generic->graph_executor_storage);
    }
    std::destroy_at(s);
  }

  void execute_impl(lf::Params &params, const lf::Context &context) const override
  {
    const ScopedNodeTimer node_timer{context, repeat_output_bnode_};

    auto &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    auto &local_user_data = *static_cast<GeoNodesLocalUserData *>(context.local_user_data);

    const NodeGeometryRepeatOutput &node_storage = *static_cast<const NodeGeometryRepeatOutput *>(
        repeat_output_bnode_.storage);
    RepeatEvalStorage &node_eval_storage = *static_cast<RepeatEvalStorage *>(context.storage);

    const int iterations = this->get_num_iterations(params);
    if (!node_eval_storage.checked_inspection_index) {
      /* Show a warning when the inspection index is out of range. */
      if (node_storage.inspection_index > 0) {
        if (node_storage.inspection_index >= iterations) {
          if (geo_eval_log::GeoTreeLogger *tree_logger = local_user_data.try_get_tree_logger(
                  user_data))
          {
            tree_logger->node_warnings.append(
                *tree_logger->allocator,
                {repeat_output_bnode_.identifier,
                 {NodeWarningType::Info, N_("Inspection index is out of range")}});
          }
        }
      }
      node_eval_storage.checked_inspection_index = true;
    }

    /* The iterations input is always used. */
    const int iterations_usage_index = zone_info_.indices.outputs.input_usages[0];
    params.set_output_if_not_set(iterations_usage_index, true);

    const NodeRepeatZoneEvalMode eval_mode = NodeRepeatZoneEvalMode(node_storage.eval_mode);
    switch (eval_mode) {
      case NODE_REPEAT_ZONE_EVAL_MODE_EAGER: {
        this->evaluate_eager(params, node_eval_storage, node_storage, user_data, iterations);
        break;
      }
      case NODE_REPEAT_ZONE_EVAL_MODE_AUTO:
      case NODE_REPEAT_ZONE_EVAL_MODE_GENERIC:
      default: {
        this->evaluate_generic(params, context, node_eval_storage, node_storage, iterations);
        break;
      }
    }
  }

  void evaluate_eager(lf::Params &params,
                      RepeatEvalStorage &node_eval_storage,
                      const NodeGeometryRepeatOutput &node_storage,
                      GeoNodesUserData &user_data,
                      const int iterations) const
  {
    const int num_repeat_items = node_storage.items_num;
    const int num_border_links = body_fn_.indices.inputs.border_links.size();

    for (const int i : IndexRange(num_repeat_items)) {
      const int lf_index = zone_info_.indices.outputs.input_usages[i + 1];
      params.set_output_if_not_set(lf_index, true);
    }
    for (const int i : IndexRange(num_border_links)) {
      const int lf_index = zone_info_.indices.outputs.border_link_usages[i];
      params.set_output_if_not_set(lf_index, true);
    }

    Array<void *, 16> input_value_ptrs(inputs_.size());
    for (const int i : inputs_.index_range()) {
      void *input_value = params.try_get_input_data_ptr_or_request(i);
      input_value_ptrs[i] = input_value;
    }
    if (input_value_ptrs.as_span().contains(nullptr)) {
      /* Wait until all inputs are ready. */
      return;
    }

    if (iterations > 50) {
      lazy_threading::send_hint();
    }

    Array<SocketValueVariant> repeat_values_a_buf(num_repeat_items);
    Array<SocketValueVariant> repeat_values_b_buf(num_repeat_items);

    MutableSpan<SocketValueVariant> repeat_values_prev = repeat_values_a_buf.as_mutable_span();
    MutableSpan<SocketValueVariant> repeat_values_next = repeat_values_b_buf.as_mutable_span();

    for (const int i : IndexRange(num_repeat_items)) {
      repeat_values_prev[i] = std::move(*static_cast<SocketValueVariant *>(
          input_value_ptrs[zone_info_.indices.inputs.main[i + 1]]));
    }

    const int body_inputs_num = body_fn_.function->inputs().size();
    const int body_outputs_num = body_fn_.function->outputs().size();

    Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> reference_sets;
    for (const auto &item : zone_info_.indices.inputs.reference_sets.items()) {
      reference_sets.add(
          item.key,
          *static_cast<const bke::GeometryNodesReferenceSet *>(input_value_ptrs[item.value]));
    }

    Vector<SocketValueVariant> border_link_values(num_border_links);
    for (const int i : IndexRange(num_border_links)) {
      border_link_values[i] = std::move(*static_cast<SocketValueVariant *>(
          input_value_ptrs[zone_info_.indices.inputs.border_links[i]]));
    }

    for (const int iteration : IndexRange(iterations)) {
      Array<GMutablePointer> body_inputs(body_inputs_num);
      Array<GMutablePointer> body_outputs(body_outputs_num);
      Array<std::optional<lf::ValueUsage>> body_input_usages(body_inputs_num);
      Array<lf::ValueUsage> body_output_usages(body_outputs_num, lf::ValueUsage::Used);
      Array<bool> body_set_outputs(body_outputs_num, false);

      SocketValueVariant iteration_value{iteration};
      bool iteration_usage_output;
      body_inputs[body_fn_.indices.inputs.main[0]] = &iteration_value;
      body_outputs[body_fn_.indices.outputs.input_usages[0]] = &iteration_usage_output;

      Array<bool> output_usage_inputs(num_repeat_items, true);
      Array<bool> input_usage_outputs(num_repeat_items);
      destruct_n(repeat_values_next.data(), num_repeat_items);
      for (const int i : IndexRange(num_repeat_items)) {
        body_inputs[body_fn_.indices.inputs.main[i + 1]] = &repeat_values_prev[i];
        body_inputs[body_fn_.indices.inputs.output_usages[i]] = &output_usage_inputs[i];

        body_outputs[body_fn_.indices.outputs.main[i]] = &repeat_values_next[i];
        body_outputs[body_fn_.indices.outputs.input_usages[i + 1]] = &input_usage_outputs[i];
      }
      Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> body_reference_sets = reference_sets;
      for (const auto &item : body_fn_.indices.inputs.reference_sets.items()) {
        const ReferenceSetIndex reference_set_i = item.key;
        body_inputs[item.value] = &body_reference_sets.lookup(reference_set_i);
      }
      Vector<SocketValueVariant> body_border_link_values = border_link_values;
      Array<bool> border_link_usages(num_border_links, false);
      for (const int i : IndexRange(num_border_links)) {
        body_inputs[body_fn_.indices.inputs.border_links[i]] = &body_border_link_values[i];
        body_outputs[body_fn_.indices.outputs.border_link_usages[i]] = &border_link_usages[i];
      }

      lf::BasicParams body_params{*body_fn_.function,
                                  body_inputs,
                                  body_outputs,
                                  body_input_usages,
                                  body_output_usages,
                                  body_set_outputs};

      const bke::RepeatZoneComputeContext body_compute_context{
          user_data.compute_context, repeat_output_bnode_, iteration};
      GeoNodesUserData body_user_data = user_data;
      body_user_data.compute_context = &body_compute_context;
      body_user_data.log_socket_values = should_log_socket_values_for_context(
          user_data, body_compute_context.hash());

      GeoNodesLocalUserData body_local_user_data{body_user_data};
      void *body_storage = body_fn_.function->init_storage(node_eval_storage.scope.allocator());
      lf::Context body_context{body_storage, &body_user_data, &body_local_user_data};
      body_fn_.function->execute(body_params, body_context);
      body_fn_.function->destruct_storage(body_storage);

      std::swap(repeat_values_prev, repeat_values_next);
    }

    const MutableSpan<SocketValueVariant> final_values = repeat_values_prev;
    for (const int i : IndexRange(num_repeat_items)) {
      params.set_output(zone_info_.indices.outputs.main[i], std::move(final_values[i]));
    }
  }

  int get_num_iterations(const lf::Params &params) const
  {
    return std::max<int>(
        0, params.get_input<SocketValueVariant>(zone_info_.indices.inputs.main[0]).get<int>());
  }

  void evaluate_generic(lf::Params &params,
                        const lf::Context &context,
                        RepeatEvalStorage &node_eval_storage,
                        const NodeGeometryRepeatOutput &node_storage,
                        const int iterations) const
  {
    if (!node_eval_storage.generic) {
      node_eval_storage.generic = &node_eval_storage.scope.construct<GenericEvalStorage>();
    }
    GenericEvalStorage &eval_storage = *node_eval_storage.generic;

    if (!eval_storage.graph_executor) {
      /* Create the execution graph in the first evaluation. */
      this->initialize_execution_graph(node_eval_storage, node_storage, iterations);
    }

    /* Execute the graph for the repeat zone. */
    lf::RemappedParams eval_graph_params{*eval_storage.graph_executor,
                                         params,
                                         eval_storage.input_index_map,
                                         eval_storage.output_index_map,
                                         eval_storage.multi_threading_enabled};
    lf::Context eval_graph_context{
        eval_storage.graph_executor_storage, context.user_data, context.local_user_data};
    eval_storage.graph_executor->execute(eval_graph_params, eval_graph_context);
  }

  /**
   * Generate a lazy-function graph that contains the loop body (`body_fn_`) as many times
   * as there are iterations. Since this graph depends on the number of iterations, it can't be
   * reused in general. We could consider caching a version of this graph per number of iterations,
   * but right now that doesn't seem worth it. In practice, it takes much less time to create the
   * graph than to execute it (for intended use cases of this generic implementation, more special
   * case repeat loop evaluations could be implemented separately).
   */
  void initialize_execution_graph(RepeatEvalStorage &node_eval_storage,
                                  const NodeGeometryRepeatOutput &node_storage,
                                  const int iterations) const
  {
    GenericEvalStorage &eval_storage = *node_eval_storage.generic;
    const int num_repeat_items = node_storage.items_num;
    const int num_border_links = body_fn_.indices.inputs.border_links.size();

    if (iterations >= 10) {
      /* Constructing and running the repeat zone has some overhead so that it's probably worth
       * trying to do something else in the meantime already. */
      lazy_threading::send_hint();
    }

    /* Take iterations input into account. */
    const int main_inputs_offset = 1;
    const int body_inputs_offset = 1;

    lf::Graph &lf_graph = eval_storage.graph;

    Vector<lf::GraphInputSocket *> lf_inputs;
    Vector<lf::GraphOutputSocket *> lf_outputs;

    for (const int i : inputs_.index_range()) {
      const lf::Input &input = inputs_[i];
      lf_inputs.append(&lf_graph.add_input(*input.type, this->input_name(i)));
    }
    for (const int i : outputs_.index_range()) {
      const lf::Output &output = outputs_[i];
      lf_outputs.append(&lf_graph.add_output(*output.type, this->output_name(i)));
    }

    /* Create body nodes. */
    VectorSet<lf::FunctionNode *> &lf_body_nodes = eval_storage.lf_body_nodes;
    for ([[maybe_unused]] const int i : IndexRange(iterations)) {
      lf::FunctionNode &lf_node = lf_graph.add_function(*body_fn_.function);
      lf_body_nodes.add_new(&lf_node);
    }

    /* Create nodes for combining border link usages. A border link is used when any of the loop
     * bodies uses the border link, so an "or" node is necessary. */
    Array<lf::FunctionNode *> lf_border_link_usage_or_nodes(num_border_links);
    eval_storage.or_function.emplace(iterations);
    for (const int i : IndexRange(num_border_links)) {
      lf::FunctionNode &lf_node = lf_graph.add_function(*eval_storage.or_function);
      lf_border_link_usage_or_nodes[i] = &lf_node;
    }

    const bool use_index_values = zone_.input_node()->output_socket(0).is_directly_linked();

    if (use_index_values) {
      eval_storage.index_values.reinitialize(iterations);
      threading::parallel_for(IndexRange(iterations), 1024, [&](const IndexRange range) {
        for (const int i : range) {
          eval_storage.index_values[i].set(i);
        }
      });
    }

    /* Handle body nodes one by one. */
    static const SocketValueVariant static_unused_index{-1};
    for (const int iter_i : lf_body_nodes.index_range()) {
      lf::FunctionNode &lf_node = *lf_body_nodes[iter_i];
      const SocketValueVariant *index_value = use_index_values ?
                                                  &eval_storage.index_values[iter_i] :
                                                  &static_unused_index;
      lf_node.input(body_fn_.indices.inputs.main[0]).set_default_value(index_value);
      for (const int i : IndexRange(num_border_links)) {
        lf_graph.add_link(*lf_inputs[zone_info_.indices.inputs.border_links[i]],
                          lf_node.input(body_fn_.indices.inputs.border_links[i]));
        lf_graph.add_link(lf_node.output(body_fn_.indices.outputs.border_link_usages[i]),
                          lf_border_link_usage_or_nodes[i]->input(iter_i));
      }

      /* Handle reference sets. */
      for (const auto &item : body_fn_.indices.inputs.reference_sets.items()) {
        lf_graph.add_link(*lf_inputs[zone_info_.indices.inputs.reference_sets.lookup(item.key)],
                          lf_node.input(item.value));
      }
    }

    static bool static_true = true;

    /* Handle body nodes pair-wise. */
    for (const int iter_i : lf_body_nodes.index_range().drop_back(1)) {
      lf::FunctionNode &lf_node = *lf_body_nodes[iter_i];
      lf::FunctionNode &lf_next_node = *lf_body_nodes[iter_i + 1];
      for (const int i : IndexRange(num_repeat_items)) {
        lf_graph.add_link(
            lf_node.output(body_fn_.indices.outputs.main[i]),
            lf_next_node.input(body_fn_.indices.inputs.main[i + body_inputs_offset]));
        /* TODO: Add back-link after being able to check for cyclic dependencies. */
        // lf_graph.add_link(lf_next_node.output(body_fn_.indices.outputs.input_usages[i]),
        //                   lf_node.input(body_fn_.indices.inputs.output_usages[i]));
        lf_node.input(body_fn_.indices.inputs.output_usages[i]).set_default_value(&static_true);
      }
    }

    /* Handle border link usage outputs. */
    for (const int i : IndexRange(num_border_links)) {
      lf_graph.add_link(lf_border_link_usage_or_nodes[i]->output(0),
                        *lf_outputs[zone_info_.indices.outputs.border_link_usages[i]]);
    }

    if (iterations > 0) {
      {
        /* Link first body node to input/output nodes. */
        lf::FunctionNode &lf_first_body_node = *lf_body_nodes[0];
        for (const int i : IndexRange(num_repeat_items)) {
          lf_graph.add_link(
              *lf_inputs[zone_info_.indices.inputs.main[i + main_inputs_offset]],
              lf_first_body_node.input(body_fn_.indices.inputs.main[i + body_inputs_offset]));
          lf_graph.add_link(
              lf_first_body_node.output(
                  body_fn_.indices.outputs.input_usages[i + body_inputs_offset]),
              *lf_outputs[zone_info_.indices.outputs.input_usages[i + main_inputs_offset]]);
        }
      }
      {
        /* Link last body node to input/output nodes. */
        lf::FunctionNode &lf_last_body_node = *lf_body_nodes.as_span().last();
        for (const int i : IndexRange(num_repeat_items)) {
          lf_graph.add_link(lf_last_body_node.output(body_fn_.indices.outputs.main[i]),
                            *lf_outputs[zone_info_.indices.outputs.main[i]]);
          lf_graph.add_link(*lf_inputs[zone_info_.indices.inputs.output_usages[i]],
                            lf_last_body_node.input(body_fn_.indices.inputs.output_usages[i]));
        }
      }
    }
    else {
      /* There are no iterations, just link the input directly to the output. */
      for (const int i : IndexRange(num_repeat_items)) {
        lf_graph.add_link(*lf_inputs[zone_info_.indices.inputs.main[i + main_inputs_offset]],
                          *lf_outputs[zone_info_.indices.outputs.main[i]]);
        lf_graph.add_link(
            *lf_inputs[zone_info_.indices.inputs.output_usages[i]],
            *lf_outputs[zone_info_.indices.outputs.input_usages[i + main_inputs_offset]]);
      }
      for (const int i : IndexRange(num_border_links)) {
        static bool static_false = false;
        lf_outputs[zone_info_.indices.outputs.border_link_usages[i]]->set_default_value(
            &static_false);
      }
    }

    lf_outputs[zone_info_.indices.outputs.input_usages[0]]->set_default_value(&static_true);

    /* The graph is ready, update the node indices which are required by the executor. */
    lf_graph.update_node_indices();

    // std::cout << "\n\n" << lf_graph.to_dot() << "\n\n";

    /* Create a mapping from parameter indices inside of this graph to parameters of the repeat
     * zone. The main complexity below stems from the fact that the iterations input is handled
     * outside of this graph. */
    eval_storage.output_index_map.reinitialize(outputs_.size() - 1);
    eval_storage.input_index_map.resize(inputs_.size() - 1);
    array_utils::fill_index_range<int>(eval_storage.input_index_map, 1);

    Vector<const lf::GraphInputSocket *> lf_graph_inputs = lf_inputs.as_span().drop_front(1);

    const int iteration_usage_index = zone_info_.indices.outputs.input_usages[0];
    array_utils::fill_index_range<int>(
        eval_storage.output_index_map.as_mutable_span().take_front(iteration_usage_index));
    array_utils::fill_index_range<int>(
        eval_storage.output_index_map.as_mutable_span().drop_front(iteration_usage_index),
        iteration_usage_index + 1);

    Vector<const lf::GraphOutputSocket *> lf_graph_outputs = lf_outputs.as_span().take_front(
        iteration_usage_index);
    lf_graph_outputs.extend(lf_outputs.as_span().drop_front(iteration_usage_index + 1));

    eval_storage.body_execute_wrapper.emplace();
    eval_storage.body_execute_wrapper->repeat_output_bnode_ = &repeat_output_bnode_;
    eval_storage.body_execute_wrapper->lf_body_nodes_ = &lf_body_nodes;
    eval_storage.side_effect_provider.emplace();
    eval_storage.side_effect_provider->repeat_output_bnode_ = &repeat_output_bnode_;
    eval_storage.side_effect_provider->lf_body_nodes_ = lf_body_nodes;

    eval_storage.graph_executor.emplace(lf_graph,
                                        std::move(lf_graph_inputs),
                                        std::move(lf_graph_outputs),
                                        nullptr,
                                        &*eval_storage.side_effect_provider,
                                        &*eval_storage.body_execute_wrapper);
    eval_storage.graph_executor_storage = eval_storage.graph_executor->init_storage(
        node_eval_storage.scope.allocator());

    /* Log graph for debugging purposes. */
    const bNodeTree &btree_orig = *DEG_get_original(&btree_);
    if (btree_orig.runtime->logged_zone_graphs) {
      std::lock_guard lock{btree_orig.runtime->logged_zone_graphs->mutex};
      btree_orig.runtime->logged_zone_graphs->graph_by_zone_id.lookup_or_add_cb(
          repeat_output_bnode_.identifier, [&]() { return lf_graph.to_dot(); });
    }
  }

  std::string input_name(const int i) const override
  {
    return zone_wrapper_input_name(zone_info_, zone_, inputs_, i);
  }

  std::string output_name(const int i) const override
  {
    return zone_wrapper_output_name(zone_info_, zone_, outputs_, i);
  }
};

LazyFunction &build_repeat_zone_lazy_function(ResourceScope &scope,
                                              const bNodeTree &btree,
                                              const bke::bNodeTreeZone &zone,
                                              ZoneBuildInfo &zone_info,
                                              const ZoneBodyFunction &body_fn)
{
  return scope.construct<LazyFunctionForRepeatZone>(btree, zone, zone_info, body_fn);
}

}  // namespace blender::nodes
