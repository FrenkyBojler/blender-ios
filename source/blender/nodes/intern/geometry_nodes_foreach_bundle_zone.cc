/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_stack.hh"
#include "BLI_string.h"

#include "FN_lazy_function_execute.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_nodes_reference_set.hh"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_geometry_nodes_list.hh"

namespace blender::nodes {

using bke::SocketValueVariant;

class SocketIndexMapBuilder {
 private:
  int bsocket_index_;
  int lf_index_;

 public:
  struct SingleSocket {
    int bsocket;
    int lf;
  };

  struct MultipleSockets {
    IndexRange bsocket;
    IndexRange lf;
  };

  SocketIndexMapBuilder(const int bsocket_index, const int lf_index)
      : bsocket_index_(bsocket_index), lf_index_(lf_index)
  {
  }

  void next(SingleSocket &r_socket)
  {
    r_socket.bsocket = bsocket_index_;
    r_socket.lf = lf_index_;
    bsocket_index_++;
    lf_index_++;
  }

  void next_extendable(MultipleSockets &r_sockets, const int amount)
  {
    r_sockets.bsocket = IndexRange::from_begin_size(bsocket_index_, amount);
    r_sockets.lf = IndexRange::from_begin_size(lf_index_, amount);
    bsocket_index_ += amount;
    /* Skip the extend socket. */
    bsocket_index_ += 1;
    lf_index_ += amount;
  }
};

struct ForeachBundleSocketIndices {
  struct {
    struct {
      SocketIndexMapBuilder::SingleSocket bundle;
      SocketIndexMapBuilder::MultipleSockets reduce;
    } in;
    struct {
      SocketIndexMapBuilder::SingleSocket subbundle;
      SocketIndexMapBuilder::SingleSocket path;
      SocketIndexMapBuilder::MultipleSockets reduce;

    } out;
  } in;
  struct {
    struct {
      SocketIndexMapBuilder::SingleSocket subbundle;
      SocketIndexMapBuilder::SingleSocket recurse;
      SocketIndexMapBuilder::MultipleSockets reduce;
      SocketIndexMapBuilder::MultipleSockets gather;
    } in;
    struct {
      SocketIndexMapBuilder::SingleSocket bundle;
      SocketIndexMapBuilder::MultipleSockets reduce;
      SocketIndexMapBuilder::MultipleSockets gather;
    } out;
  } out;
};

class ForeachBundleExecutor {
 private:
  const ZoneBodyFunction &body_fn_;
  const ForeachBundleSocketIndices &indices_;
  const bNode &output_bnode_;
  const GeoNodesUserData &user_data_;
  const Span<SocketValueVariant> border_link_values_;
  Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> reference_sets_;
  MutableSpan<SocketValueVariant> reduce_values_;
  MutableSpan<Vector<SocketValueVariant>> gather_values_;

 public:
  ForeachBundleExecutor(
      const ZoneBodyFunction &body_fn,
      const ForeachBundleSocketIndices &indices,
      const bNode &output_bnode,
      const GeoNodesUserData &user_data,
      const Span<SocketValueVariant> border_link_values,
      const Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> &reference_sets,
      MutableSpan<SocketValueVariant> reduce_values,
      MutableSpan<Vector<SocketValueVariant>> gather_values)
      : body_fn_(body_fn),
        indices_(indices),
        output_bnode_(output_bnode),
        user_data_(user_data),
        border_link_values_(border_link_values),
        reference_sets_(reference_sets),
        reduce_values_(reduce_values),
        gather_values_(gather_values)
  {
  }

  BundlePtr execute(BundlePtr root_bundle_ptr)
  {
    ZoneBodyResult initial_result = this->evaluate_zone_body(root_bundle_ptr, "");
    root_bundle_ptr = std::move(initial_result.subbundle);
    if (!root_bundle_ptr) {
      return {};
    }
    Stack<std::string> paths_to_process;
    auto add_paths = [&](Vector<std::string> &&paths) {
      /* Add in reverse order so that the first one will be on top of the stack. */
      for (const int i : paths.index_range()) {
        paths_to_process.push(std::move(paths.last(i)));
      }
    };
    add_paths(std::move(initial_result.recurse_paths));
    while (!paths_to_process.is_empty()) {
      std::string path = paths_to_process.pop();
      Bundle &root_bundle = root_bundle_ptr.ensure_mutable_inplace();
      std::optional<BundlePtr> subbundle = root_bundle.lookup_path<BundlePtr>(path);
      if (!subbundle) {
        continue;
      }
      root_bundle.remove_path(path);

      ZoneBodyResult result = this->evaluate_zone_body(std::move(*subbundle), path);
      BundlePtr new_subbundle = std::move(result.subbundle);
      if (new_subbundle) {
        add_paths(std::move(result.recurse_paths));
      }
      root_bundle.add_path_override(path, std::move(new_subbundle));
    }
    return root_bundle_ptr;
  }

 private:
  struct ZoneBodyResult {
    BundlePtr subbundle;
    Vector<std::string> recurse_paths;
  };

  ZoneBodyResult evaluate_zone_body(BundlePtr subbundle, std::string path)
  {
    ResourceScope scope;
    LinearAllocator<> &allocator = scope.allocator();

    Vector<std::string> old_subbundle_keys;
    if (subbundle) {
      for (const auto &item : subbundle->items()) {
        if (item.value.as_pointer<BundlePtr>()) {
          old_subbundle_keys.append(item.key);
        }
      }
    }

    const LazyFunction &fn = *body_fn_.function;

    Array<GMutablePointer> body_inputs(fn.inputs().size());
    Array<GMutablePointer> body_outputs(fn.outputs().size());
    Array<std::optional<lf::ValueUsage>> body_input_usages(fn.inputs().size());
    Array<lf::ValueUsage> body_output_usages(fn.outputs().size(), lf::ValueUsage::Used);
    Array<bool> body_set_outputs(fn.outputs().size(), false);

    SocketValueVariant subbundle_input_value = SocketValueVariant::From(subbundle);
    body_inputs[indices_.in.out.subbundle.lf] = &subbundle_input_value;
    SocketValueVariant path_value = SocketValueVariant::From(path);
    body_inputs[indices_.in.out.path.lf] = &path_value;
    for (const int i : reduce_values_.index_range()) {
      body_inputs[indices_.in.out.reduce.lf[i]] = &reduce_values_[i];
    }
    Array<SocketValueVariant> border_link_inputs = border_link_values_;
    for (const int i : border_link_inputs.index_range()) {
      body_inputs[body_fn_.indices.inputs.border_links[i]] = &border_link_inputs[i];
    }
    for (const int i : body_fn_.indices.inputs.output_usages) {
      body_inputs[i] = &scope.construct<bool>(true);
    }
    Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> body_reference_sets = reference_sets_;
    for (const auto &item : body_fn_.indices.inputs.reference_sets.items()) {
      body_inputs[item.value] = &body_reference_sets.lookup(item.key);
    }

    SocketValueVariant subbundle_output_value;
    std::destroy_at(&subbundle_output_value);
    body_outputs[indices_.out.in.subbundle.lf] = &subbundle_output_value;
    SocketValueVariant recurse_output_value;
    std::destroy_at(&recurse_output_value);
    body_outputs[indices_.out.in.recurse.lf] = &recurse_output_value;
    Array<SocketValueVariant> new_reduce_values(reduce_values_.size(), NoInitialization{});
    for (const int i : new_reduce_values.index_range()) {
      body_outputs[indices_.out.in.reduce.lf[i]] = &new_reduce_values[i];
    }
    Array<SocketValueVariant> new_gather_values(gather_values_.size(), NoInitialization{});
    for (const int i : new_gather_values.index_range()) {
      body_outputs[indices_.out.in.gather.lf[i]] = &new_gather_values[i];
    }
    Array<bool> border_link_usages(border_link_values_.size());
    for (const int i : border_link_usages.index_range()) {
      body_outputs[body_fn_.indices.outputs.border_link_usages[i]] = &border_link_usages[i];
    }
    Array<bool> input_usages(body_fn_.indices.outputs.input_usages.size());
    for (const int i : input_usages.index_range()) {
      body_outputs[body_fn_.indices.outputs.input_usages[i]] = &input_usages[i];
    }

    lf::BasicParams body_params{*body_fn_.function,
                                body_inputs,
                                body_outputs,
                                body_input_usages,
                                body_output_usages,
                                body_set_outputs};

    GeoNodesUserData body_user_data = user_data_;
    bke::ForeachBundleComputeContext body_compute_context{
        user_data_.compute_context, output_bnode_.identifier, std::move(path)};
    body_user_data.compute_context = &body_compute_context;
    body_user_data.log_socket_values = should_log_socket_values_for_context(
        user_data_, body_compute_context.hash());
    GeoNodesLocalUserData body_local_user_data{body_user_data};

    void *body_storage = fn.init_storage(allocator);
    lf::Context body_context(body_storage, &body_user_data, &body_local_user_data);
    fn.execute(body_params, body_context);
    fn.destruct_storage(body_storage);

    for (const int i : reduce_values_.index_range()) {
      reduce_values_[i] = std::move(new_reduce_values[i]);
    }
    for (const int i : gather_values_.index_range()) {
      gather_values_[i].append(std::move(new_gather_values[i]));
    }

    ZoneBodyResult result;
    result.subbundle = subbundle_output_value.extract<BundlePtr>();
    if (recurse_output_value.get<bool>()) {
      /* Sort keys to make the order deterministic. Otherwise it would depend on the order in a
       * hash table. */
      std::sort(old_subbundle_keys.begin(),
                old_subbundle_keys.end(),
                [](const StringRefNull a, const StringRefNull b) {
                  return BLI_strcasecmp_natural(a.c_str(), b.c_str()) < 0;
                });
      /* By only recursing into previously existing subbundles, infinite loops are avoided where
       * each iteration adds more bundles to recurse into. This behavior could become configurable
       * in the future. */
      result.recurse_paths = std::move(old_subbundle_keys);
    }
    return result;
  }
};

class LazyFunctionForForeachBundleZone : public LazyFunction {
 private:
  const bNodeTree &btree_;
  const bke::bNodeTreeZone &zone_;
  const bNode &input_bnode_;
  const bNode &output_bnode_;
  const ZoneBuildInfo &zone_info_;
  const ZoneBodyFunction &body_fn_;

  ForeachBundleSocketIndices indices_;

  friend ForeachBundleExecutor;

 public:
  LazyFunctionForForeachBundleZone(const bNodeTree &btree,
                                   const bke::bNodeTreeZone &zone,
                                   ZoneBuildInfo &zone_info,
                                   const ZoneBodyFunction &body_fn)
      : btree_(btree),
        zone_(zone),
        input_bnode_(*zone.input_node()),
        output_bnode_(*zone.output_node()),
        zone_info_(zone_info),
        body_fn_(body_fn)
  {
    debug_name_ = "Foreach Bundle";
    initialize_zone_wrapper(zone, zone_info, body_fn, true, inputs_, outputs_);
    /* All inputs are always used for now. */
    for (const int i : inputs_.index_range()) {
      inputs_[i].usage = lf::ValueUsage::Used;
    }

    const auto &node_storage = *static_cast<const NodeForeachBundleOutput *>(
        output_bnode_.storage);
    const int reduce_items_num = node_storage.reduce_items.items_num;
    const int gather_items_num = node_storage.gather_items.items_num;

    SocketIndexMapBuilder builder_in_in(0, zone_info.indices.inputs.main[0]);
    builder_in_in.next(indices_.in.in.bundle);
    builder_in_in.next_extendable(indices_.in.in.reduce, reduce_items_num);

    SocketIndexMapBuilder builder_in_out(0, body_fn.indices.inputs.main[0]);
    builder_in_out.next(indices_.in.out.subbundle);
    builder_in_out.next(indices_.in.out.path);
    builder_in_out.next_extendable(indices_.in.out.reduce, reduce_items_num);

    SocketIndexMapBuilder builder_out_in(0, body_fn.indices.outputs.main[0]);
    builder_out_in.next(indices_.out.in.subbundle);
    builder_out_in.next(indices_.out.in.recurse);
    builder_out_in.next_extendable(indices_.out.in.reduce, reduce_items_num);
    builder_out_in.next_extendable(indices_.out.in.gather, gather_items_num);

    SocketIndexMapBuilder builder_out_out(0, zone_info.indices.outputs.main[0]);
    builder_out_out.next(indices_.out.out.bundle);
    builder_out_out.next_extendable(indices_.out.out.reduce, reduce_items_num);
    builder_out_out.next_extendable(indices_.out.out.gather, gather_items_num);
  }

  void execute_impl(lf::Params &params, const lf::Context &context) const override
  {
    const ScopedNodeTimer node_timer{context, output_bnode_};

    const auto &node_storage = *static_cast<const NodeForeachBundleOutput *>(
        output_bnode_.storage);

    auto &user_data = *static_cast<GeoNodesUserData *>(context.user_data);

    const int border_link_num = body_fn_.indices.inputs.border_links.size();
    Array<SocketValueVariant> border_link_values(border_link_num);
    for (const int i : border_link_values.index_range()) {
      border_link_values[i] = params.extract_input<SocketValueVariant>(
          zone_info_.indices.inputs.border_links[i]);
    }
    Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> reference_sets;
    for (const auto &item : zone_info_.indices.inputs.reference_sets.items()) {
      reference_sets.add(item.key,
                         params.extract_input<bke::GeometryNodesReferenceSet>(item.value));
    }

    SocketValueVariant root_bundle_value = params.extract_input<SocketValueVariant>(
        indices_.in.in.bundle.lf);

    Array<SocketValueVariant> reduce_values(node_storage.reduce_items.items_num);
    for (const int i : reduce_values.index_range()) {
      reduce_values[i] = params.extract_input<SocketValueVariant>(indices_.in.in.reduce.lf[i]);
    }

    Array<Vector<SocketValueVariant>> gather_values(node_storage.gather_items.items_num);

    ForeachBundleExecutor executor(body_fn_,
                                   indices_,
                                   output_bnode_,
                                   user_data,
                                   border_link_values,
                                   reference_sets,
                                   reduce_values,
                                   gather_values);
    BundlePtr result_bundle = executor.execute(root_bundle_value.extract<BundlePtr>());

    params.set_output(indices_.out.out.bundle.lf,
                      SocketValueVariant::From(std::move(result_bundle)));
    for (const int i : reduce_values.index_range()) {
      params.set_output(indices_.out.out.reduce.lf[i], std::move(reduce_values[i]));
    }
    for (const int i : gather_values.index_range()) {
      auto *values = new ImplicitSharedValue<Vector<SocketValueVariant>>();
      values->data = std::move(gather_values[i]);
      List::ArrayData array_data = {values->data.data(), ImplicitSharingPtr<>(values)};
      ListPtr list = List::create(
          CPPType::get<SocketValueVariant>(), std::move(array_data), values->data.size());
      params.set_output(indices_.out.out.gather.lf[i], SocketValueVariant::From(std::move(list)));
    }
    for (const int i : zone_info_.indices.outputs.border_link_usages) {
      params.set_output(i, true);
    }
    for (const int i : zone_info_.indices.outputs.input_usages) {
      params.set_output(i, true);
    }
  }
};

LazyFunction &build_foreach_bundle_zone_lazy_function(ResourceScope &scope,
                                                      const bNodeTree &btree,
                                                      const bke::bNodeTreeZone &zone,
                                                      ZoneBuildInfo &zone_info,
                                                      const ZoneBodyFunction &body_fn)
{
  return scope.construct<LazyFunctionForForeachBundleZone>(btree, zone, zone_info, body_fn);
}

}  // namespace blender::nodes
