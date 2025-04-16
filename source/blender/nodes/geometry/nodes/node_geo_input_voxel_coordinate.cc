/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_voxel_coordinate_cc {

class VoxelCoordinateFieldInput final : public FieldInput {
 public:
  VoxelCoordinateFieldInput() : FieldInput(CPPType::get<float3>(), "Voxel Coordinate")
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
      return bke::voxel_coordinate_varray(*grid_leaf_context);
    }
    return {};
  }

  uint64_t hash() const override
  {
    return 524274428361;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const VoxelCoordinateFieldInput *>(&other) != nullptr;
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Voxel Coordinate").field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> coord_field{std::make_shared<VoxelCoordinateFieldInput>()};
  params.set_output("Voxel Coordinate", std::move(coord_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputVoxelCoordinate");
  ntype.ui_name = "Voxel Coordinate";
  ntype.ui_description = "Coordinate of a grid voxel";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_voxel_coordinate_cc
