/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_bounds_types.hh"
#include "BLI_math_vector_types.hh"

#include "UI_resources.hh"

#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** COMPOSITE ******************** */

namespace blender::nodes::node_composite_composite_cc {

static void cmp_node_composite_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Image")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .structure_type(StructureType::Dynamic);
}

using namespace blender::compositor;

class CompositeOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override {}

  void execute_clear() {}

  void execute_copy()
  {
    if (this->context().use_gpu()) {
      this->execute_copy_gpu();
    }
    else {
      this->execute_copy_cpu();
    }
  }

  void execute_copy_gpu() {}

  void execute_copy_cpu() {}

  /* Returns the bounds of the area of the compositing region. Only write into the compositing
   * region, which might be limited to a smaller region of the output result. */
  Bounds<int2> get_output_bounds()
  {
    return int2(0, 0);
  }

  /* The operation domain has the same size as the compositing region without any transformations
   * applied. */
  Domain compute_domain() override
  {
    return Domain(this->context().get_compositing_region_size());
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new CompositeOperation(context, node);
}

}  // namespace blender::nodes::node_composite_composite_cc

static void register_node_type_cmp_composite()
{
  namespace file_ns = blender::nodes::node_composite_composite_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeComposite", CMP_NODE_COMPOSITE_DEPRECATED);
  ntype.ui_name = "Composite";
  ntype.ui_description = "Final render output";
  ntype.enum_name_legacy = "COMPOSITE";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::cmp_node_composite_declare;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;
  ntype.no_muting = true;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_composite)
