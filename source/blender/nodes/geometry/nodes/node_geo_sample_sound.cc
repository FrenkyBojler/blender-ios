/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_socket_usage_inference.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_sound_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Sound>("Sound").optional_label();
  b.add_input<decl::Float>("Time").subtype(PROP_TIME_ABSOLUTE);
  b.add_input<decl::Bool>("All Channels").default_value(true);
  b.add_input<decl::Int>("Channel").min(0).usage_inference(
      [](const socket_usage_inference::SocketUsageParams &params) -> std::optional<bool> {
        if (const std::optional<bool> any_output_used = params.any_output_is_used()) {
          if (!*any_output_used) {
            return false;
          }
        }
        else {
          return std::nullopt;
        }
        const std::optional<bool> all_channels =
            params.get_input("All Channels").get_if_primitive<bool>();
        if (!all_channels.has_value()) {
          return true;
        }
        return !*all_channels;
      });
  b.add_input<decl::Float>("Low").subtype(PROP_FREQUENCY).default_value(0.0f).min(0.0f);
  b.add_input<decl::Float>("High").subtype(PROP_FREQUENCY).default_value(10'000.0f).min(0.0f);
  b.add_output<decl::Float>("Amplitude").reference_pass_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSampleSound");
  ntype.ui_name = "Sample Sound";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_sound_cc
