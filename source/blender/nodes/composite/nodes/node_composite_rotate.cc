/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_math_angle_types.hh"
#include "BLI_math_matrix.hh"

#include "DNA_node_types.h"

#include "RNA_enum_types.hh"

#include "BKE_node.hh"

#include "COM_domain.hh"
#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_rotate_cc {

static void cmp_node_rotate_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);

  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Angle").default_value(0.0f).min(-10000.0f).max(10000.0f).subtype(
      PROP_ANGLE);

  PanelDeclarationBuilder &sampling_panel = b.add_panel("Sampling").default_closed(true);
  sampling_panel.add_input<decl::Menu>("Interpolation")
      .default_value(CMP_NODE_INTERPOLATION_BILINEAR)
      .static_items(rna_enum_node_compositor_interpolation_items)
      .description("Interpolation method");
  sampling_panel.add_input<decl::Menu>("Extension X")
      .default_value(CMP_NODE_EXTENSION_MODE_CLIP)
      .static_items(rna_enum_node_compositor_extension_items)
      .description("The extension mode applied to the X axis");
  sampling_panel.add_input<decl::Menu>("Extension Y")
      .default_value(CMP_NODE_EXTENSION_MODE_CLIP)
      .static_items(rna_enum_node_compositor_extension_items)
      .description("The extension mode applied to the Y axis");
}

static void node_composit_init_rotate(bNodeTree * /*ntree*/, bNode *node)
{
  /* Unused, kept for forward compatibility. */
  NodeRotateData *data = MEM_callocN<NodeRotateData>(__func__);
  node->storage = data;
}

using namespace blender::compositor;

class RotateOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const math::AngleRadian rotation = this->get_input("Angle").get_single_value_default(0.0f);
    const float3x3 transformation = math::from_rotation<float3x3>(rotation);

    const Result &input = this->get_input("Image");
    Result &output = this->get_result("Image");
    output.share_data(input);
    output.transform(transformation);
    output.get_sampling_options() = this->get_options();
  }

  math::SamplingOptions get_options() const
  {
    math::SamplingOptions ret;

    switch (static_cast<CMPNodeInterpolation>(
        this->get_input("Interpolation")
            .get_single_value_default(MenuValue(CMP_NODE_INTERPOLATION_BILINEAR))
            .value))
    {
      case CMP_NODE_INTERPOLATION_ANISOTROPIC:
        ret.sampler = math::Sampler::Anisotropic;
        break;
      case CMP_NODE_INTERPOLATION_NEAREST:
        ret.sampler = math::Sampler::Nearest;
        break;
      default:
        ret.sampler = math::Sampler::Box;
        break;
      case CMP_NODE_INTERPOLATION_BICUBIC:
        ret.sampler = math::Sampler::Bspline;
        break;
    }

    switch (static_cast<CMPExtensionMode>(
        this->get_input("Extension X")
            .get_single_value_default(MenuValue(CMP_NODE_EXTENSION_MODE_CLIP))
            .value))
    {
      default:  // case CMP_NODE_EXTENSION_MODE_CLIP:
        ret.wrap_x = math::InterpWrapMode::Border;
        break;
      case CMP_NODE_EXTENSION_MODE_REPEAT:
        ret.wrap_x = math::InterpWrapMode::Repeat;
        break;
      case CMP_NODE_EXTENSION_MODE_EXTEND:
        ret.wrap_x = math::InterpWrapMode::Extend;
        break;
    }

    switch (static_cast<CMPExtensionMode>(
        this->get_input("Extension Y")
            .get_single_value_default(MenuValue(CMP_NODE_EXTENSION_MODE_CLIP))
            .value))
    {
      default:  // case CMP_NODE_EXTENSION_MODE_CLIP:
        ret.wrap_y = math::InterpWrapMode::Border;
        break;
      case CMP_NODE_EXTENSION_MODE_REPEAT:
        ret.wrap_y = math::InterpWrapMode::Repeat;
        break;
      case CMP_NODE_EXTENSION_MODE_EXTEND:
        ret.wrap_y = math::InterpWrapMode::Extend;
        break;
    }

    return ret;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new RotateOperation(context, node);
}

}  // namespace blender::nodes::node_composite_rotate_cc

static void register_node_type_cmp_rotate()
{
  namespace file_ns = blender::nodes::node_composite_rotate_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeRotate", CMP_NODE_ROTATE);
  ntype.ui_name = "Rotate";
  ntype.ui_description = "Rotate image by specified angle";
  ntype.enum_name_legacy = "ROTATE";
  ntype.nclass = NODE_CLASS_DISTORT;
  ntype.declare = file_ns::cmp_node_rotate_declare;
  ntype.initfunc = file_ns::node_composit_init_rotate;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;
  blender::bke::node_type_storage(
      ntype, "NodeRotateData", node_free_standard_storage, node_copy_standard_storage);

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_rotate)
