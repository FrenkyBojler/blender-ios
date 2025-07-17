/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_rna_define.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_rotate_rotation_cc {

enum class RotationSpace {
  Global = 0,
  Local = 1,
};

static const EnumPropertyItem space_items[] = {
    {int(RotationSpace::Global),
     "GLOBAL",
     ICON_NONE,
     "Global",
     "Rotate the input rotation in global space"},
    {int(RotationSpace::Local),
     "LOCAL",
     ICON_NONE,
     "Local",
     "Rotate the input rotation in its local space"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  b.is_function_node();
  b.add_input<decl::Menu>("Space")
      .static_items(space_items)
      .description("Base orientation for the rotation");
  b.add_input<decl::Rotation>("Rotation");
  b.add_output<decl::Rotation>("Rotation").align_with_previous();
  b.add_input<decl::Rotation>("Rotate By");
};

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI3_SO<int, math::Quaternion, math::Quaternion, math::Quaternion>(
      "Rotate Rotation Global", [](int space, math::Quaternion a, math::Quaternion b) {
        if (RotationSpace(space) == RotationSpace::Global) {
          return b * a;
        }
        return a * b;
      });
  builder.set_matching_fn(fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  fn_node_type_base(&ntype, "FunctionNodeRotateRotation", FN_NODE_ROTATE_ROTATION);
  ntype.ui_name = "Rotate Rotation";
  ntype.enum_name_legacy = "ROTATE_ROTATION";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_rotate_rotation_cc
