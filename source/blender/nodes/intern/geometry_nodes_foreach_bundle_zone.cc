/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_lazy_function.hh"

#include "NOD_geometry_nodes_bundle.hh"

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
    /* All reduce inputs are always used for now. */
    for (const int i : zone_info.indices.inputs.main) {
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
    const ScopedNodeTimer node_timer{context, output_bnode_};

    auto &user_data = *static_cast<GeoNodesUserData *>(context.user_data);
    auto &local_user_data = *static_cast<GeoNodesLocalUserData *>(context.local_user_data);

    const auto &node_storage = *static_cast<const NodeForeachBundleOutput *>(
        output_bnode_.storage);

    geo_eval_log::GeoTreeLogger *tree_logger = local_user_data.try_get_tree_logger(user_data);

    BundlePtr root_bundle =
        params.extract_input<SocketValueVariant>(indices_.in.in.bundle.lf).extract<BundlePtr>();
    params.set_output(indices_.out.out.bundle.lf, SocketValueVariant::From(root_bundle));
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
