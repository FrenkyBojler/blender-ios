/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "NOD_geometry_nodes_closure_eval.hh"
#include "NOD_geometry_nodes_lazy_function.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_nodes_reference_set.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"
#include "BKE_node_tree_reference_lifetimes.hh"

#include "NOD_geo_closure.hh"
#include "NOD_geometry_nodes_closure.hh"
#include "NOD_geometry_nodes_values.hh"

#include "DEG_depsgraph_query.hh"

#include "FN_lazy_function_execute.hh"

#include "BLI_string_utf8_symbols.h"

namespace blender::nodes {

using bke::node_tree_reference_lifetimes::ReferenceSetInfo;
using bke::node_tree_reference_lifetimes::ReferenceSetType;


uint64_t MemoryZoneSignatureKey::hash() const
{
  return 0;
}

friend bool MemoryZoneSignatureKey::operator!=(const MemoryZoneSignatureKey &a, const MemoryZoneSignatureKey &b)
{
  return false;
}

class LazyFunctionForMemoryZone : public LazyFunction {
 private:
  const bNodeTree &btree_;
  const bke::bNodeTreeZone &zone_;
  const bNode &output_bnode_;
  const ZoneBuildInfo &zone_info_;
  const ZoneBodyFunction &body_fn_;

 public:
  LazyFunctionForMemoryZone(const bNodeTree &btree,
                            const bke::bNodeTreeZone &zone,
                            ZoneBuildInfo &zone_info,
                            const ZoneBodyFunction &body_fn)
      : btree_(btree),
        zone_(zone),
        output_bnode_(*zone.output_node()),
        zone_info_(zone_info),
        body_fn_(body_fn)
  {
    debug_name_ = "Memory Zone";
    // allow_missing_requested_inputs_ = true;
    initialize_zone_wrapper(zone, zone_info, body_fn, true, inputs_, outputs_);

    for (auto &input : inputs_) {
      input.usage = lf::ValueUsage::Maybe;
    }
  }

  struct EvalState {
    std::optional<Array<bool>> set_outputs;
    void *body_state;
  };

  void execute_impl(lf::Params &params, const lf::Context &context) const override
  {
    auto &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    auto &local_user_data = *static_cast<GeoNodesLocalUserData *>(context.local_user_data);

    BLI_assert(user_data.call_data);
    auto &memory_zones_cache = user_data.call_data->memory_zones_cache;

    const int total_inputs = body_fn_.function->inputs().size();
    const int total_outputs = body_fn_.function->outputs().size();

    auto &eval_storage = *static_cast<EvalState *>(context.storage);
    if (!eval_storage.set_outputs.has_value()) {
      eval_storage.set_outputs.emplace(total_outputs, false);
    }

    Array<lf::ValueUsage> output_usages(total_outputs);
    for (const int i : IndexRange(total_outputs)) {
      output_usages[i] = params.get_output_usage(i);
    }

    Array<GMutablePointer> inputs(total_inputs);
    for (const int i : IndexRange(total_inputs)) {
      inputs[i] = GMutablePointer(inputs_[i].type, params.try_get_input_data_ptr(i));
    }

    Array<GMutablePointer> outputs(total_outputs);
    for (const int i : IndexRange(total_outputs)) {
      if ((*eval_storage.set_outputs)[i]) {
        continue;
      }
      outputs[i] = GMutablePointer(outputs_[i].type, params.get_output_data_ptr(i));
    }

    Array<std::optional<lf::ValueUsage>> input_usages(body_fn_.function->inputs().size(), std::nullopt);

    lf::BasicParams captured_params(*body_fn_.function,
                                    inputs.as_span(),
                                    outputs.as_span(),
                                    input_usages.as_mutable_span(),
                                    output_usages.as_span(),
                                    eval_storage.set_outputs->as_mutable_span());

    Vector<int> input_mapping;
    input_mapping.extend(zone_info_.indices.inputs.main);
    input_mapping.extend(zone_info_.indices.inputs.border_links);
    input_mapping.extend(zone_info_.indices.inputs.output_usages);
    Vector<int> output_mapping;
    output_mapping.extend(zone_info_.indices.outputs.input_usages);
    output_mapping.extend(zone_info_.indices.outputs.border_link_usages);
    output_mapping.extend(zone_info_.indices.outputs.main);

    bool use_threading = true;
    lf::RemappedParams mapped_params{*body_fn_.function,
                                     captured_params,
                                     input_mapping,
                                     output_mapping,
                                     use_threading};

    bke::NodeComputeContext compute_context(user_data.compute_context, output_bnode_.identifier, &btree_);

    GeoNodesUserData group_user_data = user_data;
    group_user_data.compute_context = &compute_context;
    group_user_data.verbose_log = should_log_verbose_in_context(user_data, compute_context.hash());

    GeoNodesLocalUserData group_local_user_data(group_user_data);
    lf::Context sub_context(eval_storage.body_state, &group_user_data, &group_local_user_data);

    body_fn_.function->execute(mapped_params, sub_context);

    for (const int i : eval_storage.set_outputs->index_range()) {
      if (params.output_was_set(i)) {
        continue;
      }
      if ((*eval_storage.set_outputs)[i]) {
        params.output_set(i);
      }
    }

    for (const int i : IndexRange(total_inputs)) {
      if (input_usages[i].value_or(lf::ValueUsage::Unused) != lf::ValueUsage::Used) {
        continue;
      }

      params.try_get_input_data_ptr_or_request(i);
    }
  }

  void *init_storage(LinearAllocator<> &allocator) const override
  {
    auto &state = *allocator.construct<EvalState>();
    state.body_state = body_fn_.function->init_storage(allocator);
    return &state;
  }

  void destruct_storage(void *storage) const override
  {
    auto &state = *static_cast<EvalState *>(storage);
    body_fn_.function->destruct_storage(state.body_state);
    std::destroy_at(&state);
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

LazyFunction &build_memory_zone_lazy_function(
    ResourceScope &scope,
    const bNodeTree &btree,
    const bke::bNodeTreeZone &zone,
    ZoneBuildInfo &zone_info,
    const ZoneBodyFunction &body_fn,
    std::shared_ptr<GeometryNodesLazyFunctionGraphInfo> &lf_graph_info)
{
  return scope.construct<LazyFunctionForMemoryZone>(
      btree, zone, zone_info, body_fn);
}

}  // namespace blender::nodes
