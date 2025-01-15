/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"
#include "BKE_physics_geometry.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_body_motion_type_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Physics").supported_type(bke::GeometryComponent::Type::Physics);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_output<decl::Geometry>("Physics").propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "motion_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(bke::PhysicsMotionType::Dynamic);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const auto motion_type = bke::PhysicsMotionType(params.node().custom1);
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Physics");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (bke::PhysicsGeometry *physics = geometry_set.get_physics_for_write()) {
      bke::PhysicsFieldContext field_context(*physics, AttrDomain::Point);
      fn::FieldEvaluator selection_evaluator{field_context, physics->bodies_num()};
      selection_evaluator.add(selection_field);
      selection_evaluator.evaluate();
      const IndexMask selection = selection_evaluator.get_evaluated_as_mask(0);
      if (selection.is_empty()) {
        return;
      }

      bke::AttributeWriter<int> motion_type_writer = physics->body_motion_types_for_write();
      selection.foreach_index(GrainSize(512), [&](const int index) {
        motion_type_writer.varray.set(index, int(motion_type));
      });
      motion_type_writer.finish();
    }
  });

  params.set_output("Physics", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static EnumPropertyItem motion_type_items[] = {
      {int(bke::PhysicsMotionType::Dynamic), "DYNAMIC", 0, "Dynamic", ""},
      {int(bke::PhysicsMotionType::Static), "STATIC", 0, "Static", ""},
      {int(bke::PhysicsMotionType::Kinematic), "KINEMATIC", 0, "Kinematic", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "motion_type",
                    "Motion Type",
                    "",
                    motion_type_items,
                    NOD_inline_enum_accessors(custom1),
                    int(bke::PhysicsMotionType::Dynamic));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSetBodyMotionType", GEO_NODE_SET_BODY_MOTION_TYPE);
  ntype.ui_name = "Set Motion Type";
  ntype.ui_description = "Set the motion type of a rigid body";
  ntype.enum_name_legacy = "SET_BODY_MOTION_TYPE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_body_motion_type_cc
