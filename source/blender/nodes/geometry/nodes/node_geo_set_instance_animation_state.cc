/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances_attributes.hh"

#include "node_geometry_util.hh"

/**
 * \file node_geo_set_instance_animation_state.cc
 *
 * "Set Instance Animation State" geometry node.
 *
 * Writes the standard animation-state attributes defined in
 * #BKE_instances_attributes.hh onto the Instance domain of the input
 * geometry. These attributes (`instance_clip_index`, `instance_clip_time`,
 * `instance_clip_weight`) form the contract between the scene graph and any
 * downstream system that performs procedural deformation — render delegates,
 * crowd cache readers, or future GN-based deform nodes.
 *
 * Design mirrors #node_geo_set_id.cc: one thin node per semantic group,
 * typed sockets, no custom storage struct needed.
 */

namespace blender::nodes::node_geo_set_instance_animation_state_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Instances"_ustr)
      .description(
          "Geometry whose instances will receive the animation state attributes. "
          "Non-instance components are passed through unchanged");
  b.add_output<decl::Geometry>("Instances"_ustr).propagate_all_geometry().align_with_previous();

  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .hide_value()
      .evaluated_geometry_field()
      .description("Instances to update. Unselected instances keep their current values");

  b.add_input<decl::Int>("Clip Index"_ustr)
      .default_value(0)
      .min(0)
      .evaluated_geometry_field()
      .description(
          "Index of the animation clip to play. Maps to '"
          "instance_clip_index' on the Instance domain");

  b.add_input<decl::Float>("Clip Time"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .subtype(PROP_TIME)
      .evaluated_geometry_field()
      .description(
          "Playback position within the clip in seconds. Maps to "
          "'instance_clip_time' on the Instance domain");

  b.add_input<decl::Float>("Clip Weight"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .evaluated_geometry_field()
      .description(
          "Blend weight of this clip in [0, 1]. 1.0 = only this clip. "
          "Maps to 'instance_clip_weight' on the Instance domain");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Instances"_ustr);

  if (!geometry_set.has_instances()) {
    params.set_output("Instances"_ustr, std::move(geometry_set));
    return;
  }

  const Field<bool> selection = params.extract_input<Field<bool>>("Selection"_ustr);
  const Field<int> clip_index = params.extract_input<Field<int>>("Clip Index"_ustr);
  const Field<float> clip_time = params.extract_input<Field<float>>("Clip Time"_ustr);
  const Field<float> clip_weight = params.extract_input<Field<float>>("Clip Weight"_ustr);

  GeometryComponent &component = geometry_set.get_component_for_write(
      GeometryComponent::Type::Instance);
  MutableAttributeAccessor attributes = *component.attributes_for_write();
  const bke::GeometryFieldContext field_context{component, AttrDomain::Instance};

  bke::try_capture_field_on_geometry(attributes,
                                     field_context,
                                     bke::instances::ATTR_INSTANCE_CLIP_INDEX,
                                     AttrDomain::Instance,
                                     selection,
                                     clip_index);

  bke::try_capture_field_on_geometry(attributes,
                                     field_context,
                                     bke::instances::ATTR_INSTANCE_CLIP_TIME,
                                     AttrDomain::Instance,
                                     selection,
                                     clip_time);

  bke::try_capture_field_on_geometry(attributes,
                                     field_context,
                                     bke::instances::ATTR_INSTANCE_CLIP_WEIGHT,
                                     AttrDomain::Instance,
                                     selection,
                                     clip_weight);

  params.set_output("Instances"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetInstanceAnimationState"_ustr);
  ntype.ui_name = "Set Instance Animation State";
  ntype.ui_description =
      "Write standard animation-state attributes (clip index, time, weight) "
      "onto instances for use by render delegates and deform evaluators";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_instance_animation_state_cc
