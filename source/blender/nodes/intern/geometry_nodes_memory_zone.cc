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

bool operator!=(const MemoryZoneSignatureKey &a, const MemoryZoneSignatureKey &b)
{/*
  if (a.output_identifier != b.output_identifier) {
    return false;
  }
  
  BLI_assert(a.inputs.size() == b.inputs.size());
  
  for (const int i : a.inputs.index_range()) {
    const bke::SocketValueVariant a_value = a.inputs[i];
    const bke::SocketValueVariant b_value = b.inputs[i];
    
    if (a_value.kind() != b_value.kind()) {
      return false;
    }
    
    BLI_assert(a_value.socket_type() == b_value.socket_type());
    BLI_assert(!a_value.is_field());
    BLI_assert(!a_value.is_volume_grid());
    
    if (a_value.is_single()) {
      const GPointer a_data = a_value.get_single_ptr();
      const GPointer b_data = b_value.get_single_ptr();
      BLI_assert(a_data.type() == b_data.type());
      if (!a_data.type().is_equal(a_data.get(), b_data.get())) {
        return false;
      }
      continue;
    }
    
    if (a_value.is_list()) {
      const auto a_data = a_value.get<GListPtr>();
      const auto b_data = b_value.get<GListPtr>();
      // TODO
      BLI_assert_unreachable();
      continue;
    }
    
    BLI_assert_unreachable();
  }
  */
  return true;
}

uint64_t MemoryZoneSignatureKey::hash() const
{
  uint64_t hash_value = this->output_identifier;
  /*
  for (const int i : this->inputs.index_range()) {
    const bke::SocketValueVariant value = this->inputs[i];
    
    BLI_assert(!value.is_field());
    BLI_assert(!value.is_volume_grid());
    
    if (value.is_single()) {
      const GPointer data = value.get_single_ptr();
      hash_value = get_default_hash(hash_value, data.type().hash_or_fallback(data.get(), 0));
      continue;
    }

    if (a_value.is_list()) {
      const auto data = value.get<GListPtr>();
      // TODO
      BLI_assert_unreachable();
      continue;
    }

    BLI_assert_unreachable();
  }
  */
  return hash_value;
}

class LazyFunctionForMemoryZone : public LazyFunction {
 private:
  const bNodeTree &btree_;
  const bke::bNodeTreeZone &zone_;
  const bNode &output_bnode_;
  const ZoneBuildInfo &zone_info_;
  const ZoneBodyFunction &body_fn_;

  mutable Vector<int> input_mapping_;
  mutable Vector<int> output_mapping_;

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

    const IndexRange main_inputs(0, zone_info_.indices.inputs.main.size());
    for (const int index : main_inputs) {
      /* Currently use all inputs as atomic key for all outputs, without laziness for inputs used for requested outputs. */
      inputs_[index].usage = lf::ValueUsage::Used;
    }
  }

  void ensure_io_mapping() const
  {
    if (!input_mapping_.is_empty()) {
      return;
    }
    input_mapping_.extend(zone_info_.indices.inputs.main);
    input_mapping_.extend(zone_info_.indices.inputs.border_links);
    input_mapping_.extend(zone_info_.indices.inputs.output_usages);
    output_mapping_.extend(zone_info_.indices.outputs.input_usages);
    output_mapping_.extend(zone_info_.indices.outputs.border_link_usages);
    output_mapping_.extend(zone_info_.indices.outputs.main);
  }

  void execute_impl(lf::Params &params, const lf::Context &context) const override
  {
    this->ensure_io_mapping();
    
    auto &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    auto &local_user_data = *static_cast<GeoNodesLocalUserData *>(context.local_user_data);

    BLI_assert(user_data.call_data);
    auto &memory_zones_cache = user_data.call_data->memory_zones_cache;

    const IndexRange main_inputs(0, zone_info_.indices.inputs.main.size());
    BLI_assert(std::all_of(main_inputs.begin(), main_inputs.end(), [&](const int i) {
      return params.try_get_input_data_ptr(i) != nullptr;
    }));

    const int total_inputs = body_fn_.function->inputs().size();
    const int total_outputs = body_fn_.function->outputs().size();

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

    Array<lf::ValueUsage> output_usages(total_outputs);
    for (const int i : IndexRange(total_outputs)) {
      output_usages[i] = params.get_output_usage(i);
    }

    Array<std::optional<lf::ValueUsage>> input_usages(body_fn_.function->inputs().size(), std::nullopt);

    Array<bool> output_is_set(total_outputs, false);

    {
      MemoryZoneSignatureKey key;
      key.output_identifier = output_bnode_.identifier;
      key.inputs.reinitialize(main_inputs.size());
      for (const int i : main_inputs) {
        key.inputs[i] = inputs[i].get<bke::SocketValueVariant>();

        if (key.inputs[i].is_list() || key.inputs[i].is_single()) {
          continue;
        }

        // TODO: error report
        BLI_assert_unreachable();
        return;
      }

      std::lock_guard lock(memory_zones_cache.mutex);
      if (MemoryZoneValue *value = memory_zones_cache.caches.lookup_ptr(key)) {
        for (const int index : value->output_values.index_range()) {
          if (output_usages[index] == lf::ValueUsage::Unused) {
            continue;
          }

          if (!value->output_values[index].has_value()) {
            continue;
          }

          output_is_set[index] = true;
          outputs[index].get<bke::SocketValueVariant>() = *value->output_values[index];
        }
      }
    }

    lf::BasicParams captured_params(*body_fn_.function,
                                    inputs.as_span(),
                                    outputs.as_span(),
                                    input_usages.as_mutable_span(),
                                    output_usages.as_span(),
                                    eval_storage.set_outputs->as_mutable_span());

    bool use_threading = true;
    lf::RemappedParams mapped_params{*body_fn_.function,
                                     captured_params,
                                     input_mapping_,
                                     output_mapping_,
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
