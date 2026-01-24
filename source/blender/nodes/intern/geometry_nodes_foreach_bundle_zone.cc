/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_lazy_function.hh"

namespace blender::nodes {

class LazyFunctionForForeachBundleZone : public LazyFunction {
 private:
  const bNodeTree &btree_;
  const bke::bNodeTreeZone &zone_;
  const bNode &output_bnode_;
  const ZoneBuildInfo &zone_info_;
  const ZoneBodyFunction &body_fn_;

 public:
  LazyFunctionForForeachBundleZone(const bNodeTree &btree,
                                   const bke::bNodeTreeZone &zone,
                                   ZoneBuildInfo &zone_info,
                                   const ZoneBodyFunction &body_fn)
      : btree_(btree),
        zone_(zone),
        output_bnode_(*zone.output_node()),
        zone_info_(zone_info),
        body_fn_(body_fn)
  {
    debug_name_ = "Foreach Bundle";
    initialize_zone_wrapper(zone, zone_info, body_fn, true, inputs_, outputs_);
  }

  void execute_impl(lf::Params &params, const lf::Context &context) const override {}
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
