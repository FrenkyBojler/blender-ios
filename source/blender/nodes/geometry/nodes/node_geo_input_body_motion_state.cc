/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_physics_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_body_motion_state_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Bool>("Dynamic").field_source();
  b.add_output<decl::Bool>("Static").field_source();
  b.add_output<decl::Bool>("Kinematic").field_source();
  b.add_output<decl::Vector>("Position").field_source();
  b.add_output<decl::Rotation>("Rotation").field_source();
  b.add_output<decl::Vector>("Velocity").field_source();
  b.add_output<decl::Vector>("Angular Velocity").field_source();
}

class MotionTypeFieldInput : public bke::GeometryFieldInput {
 private:
  bke::PhysicsMotionType motion_type_;

 public:
  MotionTypeFieldInput(const bke::PhysicsMotionType motion_type)
      : GeometryFieldInput(CPPType::get<bool>(), "motion type input field"),
        motion_type_(motion_type)
  {
    category_ = Category::NamedAttribute;
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const override
  {
    if (auto *physics = context.physics()) {
      return VArray<bool>::ForFunc(physics->bodies_num(),
                                   [motion_types = physics->body_motion_types(),
                                    motion_type = motion_type_](const int index) {
                                     return bke::PhysicsMotionType(motion_types[index]) ==
                                            motion_type;
                                   });
    }
    return {};
  }

  std::string socket_inspection_name() const override
  {
    return TIP_("Motion type from physics");
  }

  uint64_t hash() const override
  {
    return 2633455837;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const MotionTypeFieldInput *other_typed = dynamic_cast<const MotionTypeFieldInput *>(
            &other))
    {
      return other_typed->motion_type_ == motion_type_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(
      const GeometryComponent & /*component*/) const override
  {
    return bke::AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const auto is_dynamic_field_node = std::make_shared<MotionTypeFieldInput>(
      bke::PhysicsMotionType::Dynamic);
  const auto is_static_field_node = std::make_shared<MotionTypeFieldInput>(
      bke::PhysicsMotionType::Static);
  const auto is_kinematic_field_node = std::make_shared<MotionTypeFieldInput>(
      bke::PhysicsMotionType::Kinematic);
  const fn::GField is_static_field(is_static_field_node);

  params.set_output("Dynamic", fn::GField(std::move(is_dynamic_field_node)));
  params.set_output("Static", fn::GField(std::move(is_static_field_node)));
  params.set_output("Kinematic", fn::GField(std::move(is_kinematic_field_node)));

  params.set_output("Position",
                    AttributeFieldInput::Create<float3>(bke::PhysicsGeometry::attribute_name(
                        bke::PhysicsGeometry::BodyAttribute::position)));
  params.set_output(
      "Rotation",
      AttributeFieldInput::Create<math::Quaternion>(
          bke::PhysicsGeometry::attribute_name(bke::PhysicsGeometry::BodyAttribute::rotation)));
  params.set_output("Velocity",
                    AttributeFieldInput::Create<float3>(bke::PhysicsGeometry::attribute_name(
                        bke::PhysicsGeometry::BodyAttribute::velocity)));
  params.set_output("Angular Velocity",
                    AttributeFieldInput::Create<float3>(bke::PhysicsGeometry::attribute_name(
                        bke::PhysicsGeometry::BodyAttribute::angular_velocity)));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputBodyMotionState", GEO_NODE_INPUT_BODY_MOTION_STATE);
  ntype.ui_name = "Body Motion State";
  ntype.ui_description = "Motion state properties of rigid bodies";
  ntype.enum_name_legacy = "INPUT_BODY_MOTION_STATE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_body_motion_state_cc
