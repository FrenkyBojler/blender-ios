/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "BKE_volume_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_voxel_center_cc {

class VoxelCenterFieldInput final : public FieldInput {
 public:
  VoxelCenterFieldInput() : FieldInput(CPPType::get<float3>(), "Voxel Center")
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const FieldContext &context,
                                 const IndexMask & /*mask*/,
                                 ResourceScope & /*scope*/) const final
  {
    if (const bke::GridLeafNodeFieldContext *grid_leaf_context =
            dynamic_cast<const bke::GridLeafNodeFieldContext *>(&context))
    {
      const float4x4 grid_transform = grid_leaf_context->transform();
      return VArray<float3>::ForFunc(
          grid_leaf_context->size(), [grid_leaf_context, grid_transform](const int64_t index) {
            const int3 coord = grid_leaf_context->index_to_global_coord(index);
            /* Offset by 0.5 to get the center position. */
            return math::transform_point(grid_transform, float3(coord) + float3(0.5f));
          });
    }
    return {};
  }

  uint64_t hash() const override
  {
    return 329300953061;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const VoxelCenterFieldInput *>(&other) != nullptr;
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Voxel Center").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> center_field{std::make_shared<VoxelCenterFieldInput>()};
  params.set_output("Voxel Center", std::move(center_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputVoxelCenter");
  ntype.ui_name = "Voxel Center";
  ntype.ui_description = "Center position of a grid voxel";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_voxel_center_cc
