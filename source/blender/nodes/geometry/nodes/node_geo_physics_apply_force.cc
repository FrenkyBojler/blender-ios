/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"
#include "BKE_physics_geometry.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_apply_force_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Physics").supported_type(bke::GeometryComponent::Type::Physics);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Vector>("Force").field_on_all();
  b.add_input<decl::Vector>("Position").hide_value().field_on_all();
  b.add_output<decl::Geometry>("Physics").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Physics");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<float3> forces_field = params.extract_input<Field<float3>>("Force");
  Field<float3> positions_field = params.extract_input<Field<float3>>("Position");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (bke::PhysicsGeometry *physics = geometry_set.get_physics_for_write()) {
      bke::PhysicsFieldContext field_context(*physics, bke::AttrDomain::Point);
      FieldEvaluator evaluator(field_context, physics->bodies_num());
      evaluator.set_selection(selection_field);
      evaluator.add(forces_field);
      evaluator.add(positions_field);
      evaluator.evaluate();

      physics->state_for_write().apply_force(evaluator.get_evaluated_selection_as_mask(),
                                             evaluator.get_evaluated<float3>(0),
                                             evaluator.get_evaluated<float3>(1));
    }
  });

  params.set_output("Physics", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "PhysicsApplyForce", GEO_NODE_PHYSICS_APPLY_FORCE);
  ntype.ui_name = "Apply Force";
  ntype.ui_description = "Apply force to rigid bodies";
  ntype.enum_name_legacy = "PHYSICS_APPLY_FORCE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;

  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_apply_force_cc
