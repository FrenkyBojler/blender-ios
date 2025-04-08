/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_cpp_types.hh"
#include "BLI_parameter_pack_utils.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"
#include "NOD_xpbd_constraints.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "node_function_util.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_fn_constraint_cc {

namespace mf = blender::fn::multi_function;

enum class ConstraintType {
  PositionGoal,
  RotationGoal,
  VelocityGoal,
  AngularVelocityGoal,
  StretchShear,
  BendTwist,
  ContactPosition,
  ContactVelocity,
};

/* Shortcuts. */
template<typename T> using mf_input = mf::ParamTag<mf::ParamCategory::SingleInput, T>;
template<typename T> using mf_output = mf::ParamTag<mf::ParamCategory::SingleOutput, T>;

template<typename ExecPreset> static auto stretch_shear_multifunction(ExecPreset exec_preset)
{
  constexpr auto param_tags = TypeSequence<mf_input<bool>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<math::Quaternion>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_output<float3>,
                                           mf_output<float3>,
                                           mf_output<float3>,
                                           mf_output<math::Quaternion>>();
  auto call_fn = mf::build::detail::build_multi_function_call_from_element_fn(
      [](const bool linearized_rotation,
         float3 lambda,
         float3 position1,
         float3 position2,
         math::Quaternion rotation,
         const float weight_pos1,
         const float weight_pos2,
         const float weight_rot,
         const float edge_length,
         const float alpha,
         float3 &lambda_out,
         float3 &position1_out,
         float3 &position2_out,
         math::Quaternion &rotation_out) -> void {
        if (linearized_rotation) {
          xpbd_constraints::apply_position_stretch_shear<true>(weight_pos1,
                                                               weight_pos2,
                                                               weight_rot,
                                                               edge_length,
                                                               alpha,
                                                               lambda,
                                                               position1,
                                                               position2,
                                                               rotation);
        }
        else {
          xpbd_constraints::apply_position_stretch_shear<false>(weight_pos1,
                                                                weight_pos2,
                                                                weight_rot,
                                                                edge_length,
                                                                alpha,
                                                                lambda,
                                                                position1,
                                                                position2,
                                                                rotation);
        }
        lambda_out = lambda;
        position1_out = position1;
        position2_out = position2;
        rotation_out = rotation;
      },
      exec_preset,
      param_tags);
  return mf::build::detail::CustomMF("XPBD Stretch/Shear Rod Constraint", call_fn, param_tags);
}

template<typename ExecPreset> static auto bend_twist_multifunction(ExecPreset exec_preset)
{
  constexpr auto param_tags = TypeSequence<mf_input<bool>,
                                           mf_input<float3>,
                                           mf_input<math::Quaternion>,
                                           mf_input<math::Quaternion>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_output<float3>,
                                           mf_output<math::Quaternion>,
                                           mf_output<math::Quaternion>>();
  auto call_fn = mf::build::detail::build_multi_function_call_from_element_fn(
      [](const bool linearized_rotation,
         float3 lambda,
         math::Quaternion rotation1,
         math::Quaternion rotation2,
         const float weight_rot1,
         const float weight_rot2,
         const float3 &darboux_vector,
         const float3 &alpha,
         float3 &lambda_out,
         math::Quaternion &rotation_out1,
         math::Quaternion &rotation_out2) -> void {
        if (linearized_rotation) {
          xpbd_constraints::apply_position_bend_twist<true>(
              weight_rot1, weight_rot2, darboux_vector, alpha, lambda, rotation1, rotation2);
        }
        else {
          xpbd_constraints::apply_position_bend_twist<false>(
              weight_rot1, weight_rot2, darboux_vector, alpha, lambda, rotation1, rotation2);
        }
        lambda_out = lambda;
        rotation_out1 = rotation1;
        rotation_out2 = rotation2;
      },
      exec_preset,
      param_tags);
  return mf::build::detail::CustomMF("XPBD Stretch/Shear Rod Constraint", call_fn, param_tags);
}

template<typename ExecPreset> static auto contact_position_multifunction(ExecPreset exec_preset)
{
  constexpr auto param_tags = TypeSequence<mf_input<bool>,
                                           mf_input<float>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<math::Quaternion>,
                                           mf_input<math::Quaternion>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float3>,
                                           mf_input<float>,
                                           mf_output<float>,
                                           mf_output<float3>,
                                           mf_output<float3>,
                                           mf_output<math::Quaternion>,
                                           mf_output<math::Quaternion>>();
  auto call_fn = mf::build::detail::build_multi_function_call_from_element_fn(
      [](const bool /*linearized_rotation*/,
         float lambda,
         float3 position1,
         float3 position2,
         math::Quaternion rotation1,
         math::Quaternion rotation2,
         const float weight_pos1,
         const float weight_pos2,
         const float weight_rot1,
         const float weight_rot2,
         const float3 &local_position1,
         const float3 &local_position2,
         const float3 &normal,
         const float alpha,
         float &lambda_out,
         float3 &position1_out,
         float3 &position2_out,
         math::Quaternion &rotation1_out,
         math::Quaternion &rotation2_out) -> void {
        xpbd_constraints::apply_position_contact(weight_pos1,
                                                 weight_pos2,
                                                 weight_rot1,
                                                 weight_rot2,
                                                 local_position1,
                                                 local_position2,
                                                 normal,
                                                 alpha,
                                                 lambda,
                                                 position1,
                                                 position2,
                                                 rotation1,
                                                 rotation2);
        lambda_out = lambda;
        position1_out = position1;
        position2_out = position2;
        rotation1_out = rotation1;
        rotation2_out = rotation2;
      },
      exec_preset,
      param_tags);
  return mf::build::detail::CustomMF("XPBD Contact Position Constraint", call_fn, param_tags);
}

static const EnumPropertyItem rna_enum_constraint_type_items[] = {
    {int(ConstraintType::PositionGoal), "POSITION_GOAL", 0, "Position Goal", ""},
    {int(ConstraintType::RotationGoal), "ROTATION_GOAL", 0, "Rotation Goal", ""},
    {int(ConstraintType::VelocityGoal), "VELOCITY_GOAL", 0, "Velocity Goal", ""},
    {int(ConstraintType::AngularVelocityGoal),
     "ANGULAR_VELOCITY_GOAL",
     0,
     "Angular Velocity Goal",
     ""},
    {int(ConstraintType::StretchShear), "STRETCH_SHEAR", 0, "Stretch/Shear", ""},
    {int(ConstraintType::BendTwist), "BEND_TWIST", 0, "Bend/Twist", ""},
    {int(ConstraintType::ContactPosition), "CONTACT_POSITION", 0, "Contact Position", ""},
    {int(ConstraintType::ContactVelocity), "CONTACT_VELOCITY", 0, "Contact Velocity", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_default_layout();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const ConstraintType constraint_type = ConstraintType(node->custom1);

  /* XXX This should not really be a multifunction input but a fixed option. Using a bool input
   * here for convenience for the time being. */
  b.add_input<decl::Bool>("Linearized Rotation")
      .description("Compute rotation offset as linear offsets, only accurate for small angles.");

  switch (constraint_type) {
    case ConstraintType::PositionGoal:
      break;
    case ConstraintType::RotationGoal:
      break;
    case ConstraintType::VelocityGoal:
      break;
    case ConstraintType::AngularVelocityGoal:
      break;
    case ConstraintType::StretchShear:
      b.add_input<decl::Vector>("Lambda");
      b.add_output<decl::Vector>("Lambda").align_with_previous();
      b.add_input<decl::Vector>("Position 1").hide_value();
      b.add_output<decl::Vector>("Position 1").align_with_previous();
      b.add_input<decl::Vector>("Position 2").hide_value();
      b.add_output<decl::Vector>("Position 2").align_with_previous();
      b.add_input<decl::Rotation>("Rotation").hide_value();
      b.add_output<decl::Rotation>("Rotation").align_with_previous();
      b.add_separator();
      b.add_input<decl::Float>("Position Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Position Weight 2").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight").default_value(1.0f);
      b.add_separator();
      b.add_input<decl::Float>("Edge Length");
      b.add_input<decl::Float>("Alpha");
      break;
    case ConstraintType::BendTwist:
      b.add_input<decl::Vector>("Lambda");
      b.add_output<decl::Vector>("Lambda").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 1").hide_value();
      b.add_output<decl::Rotation>("Rotation 1").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 2").hide_value();
      b.add_output<decl::Rotation>("Rotation 2").align_with_previous();
      b.add_separator();
      b.add_input<decl::Float>("Rotation Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight 2").default_value(1.0f);
      b.add_separator();
      b.add_input<decl::Float>("Edge Length");
      b.add_input<decl::Vector>("Darboux Vector");
      b.add_input<decl::Vector>("Alpha");
      break;
    case ConstraintType::ContactPosition:
      b.add_input<decl::Float>("Lambda");
      b.add_output<decl::Float>("Lambda").align_with_previous();
      b.add_input<decl::Vector>("Position 1").hide_value();
      b.add_output<decl::Vector>("Position 1").align_with_previous();
      b.add_input<decl::Vector>("Position 2").hide_value();
      b.add_output<decl::Vector>("Position 2").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 1").hide_value();
      b.add_output<decl::Rotation>("Rotation 1").align_with_previous();
      b.add_input<decl::Rotation>("Rotation 2").hide_value();
      b.add_output<decl::Rotation>("Rotation 2").align_with_previous();
      b.add_separator();
      b.add_input<decl::Float>("Position Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Position Weight 2").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight 1").default_value(1.0f);
      b.add_input<decl::Float>("Rotation Weight 2").default_value(1.0f);
      b.add_separator();
      b.add_input<decl::Vector>("Local Position 1");
      b.add_input<decl::Vector>("Local Position 2");
      b.add_input<decl::Vector>("Normal");
      b.add_input<decl::Float>("Alpha");
      break;
    case ConstraintType::ContactVelocity:
      break;
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "constraint_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(ConstraintType::PositionGoal);
}

static const mf::MultiFunction *get_multi_function(const bNode &bnode)
{
  namespace mf = fn::multi_function;

  const ConstraintType constraint_type = ConstraintType(bnode.custom1);
  static auto exec_preset = mf::build::exec_presets::AllSpanOrSingle();

  static auto fn_stretch_shear = stretch_shear_multifunction(exec_preset);
  static auto fn_bend_twist = bend_twist_multifunction(exec_preset);
  static auto fn_contact = contact_position_multifunction(exec_preset);

  switch (constraint_type) {
    case ConstraintType::PositionGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintType::RotationGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintType::VelocityGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintType::AngularVelocityGoal:
      BLI_assert_unreachable();
      return nullptr;
    case ConstraintType::StretchShear:
      return &fn_stretch_shear;
    case ConstraintType::BendTwist:
      return &fn_bend_twist;
    case ConstraintType::ContactPosition:
      return &fn_contact;
    case ConstraintType::ContactVelocity:
      BLI_assert_unreachable();
      return nullptr;
    default:
      BLI_assert_unreachable();
      return nullptr;
  }
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "constraint_type",
                    "Constraint Type",
                    "",
                    rna_enum_constraint_type_items,
                    NOD_inline_enum_accessors(custom1),
                    int(ConstraintType::PositionGoal));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  fn_node_type_base(&ntype, "FunctionNodeConstraint", FN_NODE_CONSTRAINT);
  ntype.ui_name = "Evaluate Constraint";
  ntype.enum_name_legacy = "CONSTRAINT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.build_multi_function = node_build_multi_function;
  ntype.draw_buttons = node_layout;
  bke::node_type_size(ntype, 200, 100, 300);
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_constraint_cc
