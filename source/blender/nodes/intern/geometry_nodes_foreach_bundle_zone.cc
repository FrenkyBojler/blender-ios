/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_nodes_reference_set.hh"
#include "BLI_stack.hh"

#include "FN_lazy_function_execute.hh"

#include "BKE_compute_contexts.hh"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_lazy_function.hh"

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

class LazyFunctionForForeachBundleZone : public LazyFunction {
 private:
  const bNodeTree &btree_;
  const bke::bNodeTreeZone &zone_;
  const bNode &input_bnode_;
  const bNode &output_bnode_;
  const ZoneBuildInfo &zone_info_;
  const ZoneBodyFunction &body_fn_;

  /** Reduces the hard-coding of index offsets in lots of places below which is quite brittle. */
  struct {
    struct {
      struct {
        SocketIndexMapBuilder::SingleSocket bundle;
        SocketIndexMapBuilder::MultipleSockets reduce;
        SocketIndexMapBuilder::SingleSocket type;
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
  } indices_;

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
    builder_in_in.next(indices_.in.in.type);

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
    ResourceScope scope;
    LinearAllocator<> &allocator = scope.allocator();
    const ScopedNodeTimer node_timer{context, output_bnode_};

    auto &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    // auto &local_user_data = *static_cast<GeoNodesLocalUserData *>(context.local_user_data);

    // const auto &node_storage = *static_cast<const NodeForeachBundleOutput *>(
    //     output_bnode_.storage);

    // geo_eval_log::GeoTreeLogger *tree_logger = local_user_data.try_get_tree_logger(user_data);

    SocketValueVariant root_bundle_value = params.extract_input<SocketValueVariant>(
        indices_.in.in.bundle.lf);

    const LazyFunction &body_fn = *body_fn_.function;

    Array<GMutablePointer> body_inputs(body_fn.inputs().size());
    Array<GMutablePointer> body_outputs(body_fn.outputs().size());
    Array<std::optional<lf::ValueUsage>> body_input_usages(body_fn.inputs().size());
    Array<lf::ValueUsage> body_output_usages(body_fn.outputs().size(), lf::ValueUsage::Used);
    Array<bool> body_set_outputs(body_fn.outputs().size(), false);

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

    Array<SocketValueVariant> border_link_inputs = border_link_values;
    SocketValueVariant subbundle_input_value = root_bundle_value;
    SocketValueVariant path_value = SocketValueVariant::From(std::string(""));
    Map<ReferenceSetIndex, bke::GeometryNodesReferenceSet> body_reference_sets = reference_sets;
    {
      body_inputs[indices_.in.out.subbundle.lf] = &subbundle_input_value;
      body_inputs[indices_.in.out.path.lf] = &path_value;
      for (const int i : border_link_inputs.index_range()) {
        body_inputs[body_fn_.indices.inputs.border_links[i]] = &border_link_inputs[i];
      }
      for (const int i : body_fn_.indices.inputs.output_usages) {
        body_inputs[i] = &scope.construct<bool>(true);
      }
      for (const auto &item : body_fn_.indices.inputs.reference_sets.items()) {
        body_inputs[item.value] = &body_reference_sets.lookup(item.key);
      }
    }
    SocketValueVariant subbundle_output_value;
    std::destroy_at(&subbundle_output_value);
    SocketValueVariant recurse_output_value;
    std::destroy_at(&recurse_output_value);

    Array<bool> input_usages(body_fn_.indices.outputs.input_usages.size());
    Array<bool> border_link_usages(border_link_num);

    {
      body_outputs[indices_.out.in.subbundle.lf] = &subbundle_output_value;
      body_outputs[indices_.out.in.recurse.lf] = &recurse_output_value;
      for (const int i : border_link_usages.index_range()) {
        body_outputs[body_fn_.indices.outputs.border_link_usages[i]] = &border_link_usages[i];
      }
      for (const int i : input_usages.index_range()) {
        body_outputs[body_fn_.indices.outputs.input_usages[i]] = &input_usages[i];
      }
    }

    lf::BasicParams body_params{*body_fn_.function,
                                body_inputs,
                                body_outputs,
                                body_input_usages,
                                body_output_usages,
                                body_set_outputs};

    GeoNodesUserData body_user_data = user_data;
    /* TODO: Use proper compute context. */
    bke::NodeComputeContext body_compute_context{
        user_data.compute_context, output_bnode_.identifier, 0};
    body_user_data.compute_context = &body_compute_context;
    body_user_data.log_socket_values = should_log_socket_values_for_context(
        user_data, body_compute_context.hash());
    GeoNodesLocalUserData body_local_user_data{body_user_data};

    void *body_storage = body_fn.init_storage(allocator);
    lf::Context body_context(body_storage, &body_user_data, &body_local_user_data);
    body_fn.execute(body_params, body_context);
    body_fn.destruct_storage(body_storage);

    params.set_output(indices_.out.out.bundle.lf, std::move(subbundle_output_value));
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
