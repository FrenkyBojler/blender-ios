/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_physics_geometry.hh"

#include "FN_field.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_body_activation_state_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Physics").supported_type(bke::GeometryComponent::Type::Physics);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Bool>("Active").default_value(true).field_on_all();
  b.add_output<decl::Geometry>("Physics").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Physics");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<bool> active_field = params.extract_input<Field<bool>>("Active");

  if (bke::PhysicsGeometry *physics = geometry_set.get_physics_for_write()) {
    bke::PhysicsFieldContext context(*physics, AttrDomain::Point);
    fn::FieldEvaluator evaluator(context, physics->bodies_num());
    evaluator.add(selection_field);
    evaluator.add(active_field);
    evaluator.evaluate();
    IndexMask selection = evaluator.get_evaluated_as_mask(0);
    VArraySpan<bool> active = evaluator.get_evaluated<bool>(1);

    bke::AttributeWriter<bool> active_writer =
        physics->attributes_for_write().lookup_for_write<bool>(
            bke::PhysicsGeometry::attribute_name(bke::PhysicsGeometry::BodyAttribute::is_active));

    selection.foreach_index(
        GrainSize(1024), [&](const int index) { active_writer.varray.set(index, active[index]); });

    active_writer.finish();
  }

  params.set_output("Physics", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "SetBodyActivationState", GEO_NODE_SET_BODY_ACTIVATION_STATE);
  ntype.ui_name = "Set Body Activation State";
  ntype.ui_description = "Set body activation state in the simulation";
  ntype.enum_name_legacy = "SET_BODY_ACTIVATION_STATE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_body_activation_state_cc
