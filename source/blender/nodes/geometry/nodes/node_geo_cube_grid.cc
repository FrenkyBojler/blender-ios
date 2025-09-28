/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_math_matrix.hh"
#include "BLI_task.hh"

#include "BKE_attribute_math.hh"
#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/Dense.h>
#endif

#include "NOD_rna_define.hh"
#include "NOD_socket.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

namespace blender::nodes::node_geo_cube_grid_cc {


static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_input(data_type, "Value")
      .description("Value field to evaluate at each grid point")
      .supports_field();
  b.add_input(data_type, "Background")
      .description("Default value for grid voxels outside the filled region");

  b.add_input<decl::Vector>("Min")
      .default_value(float3(-1.0f))
      .description("Minimum boundary of the grid");
  b.add_input<decl::Vector>("Max")
      .default_value(float3(1.0f))
      .description("Maximum boundary of the grid");

  b.add_input<decl::Int>("Resolution X")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the X axis");
  b.add_input<decl::Int>("Resolution Y")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the Y axis");
  b.add_input<decl::Int>("Resolution Z")
      .default_value(32)
      .min(2)
      .description("Number of voxels in the Z axis");

  b.add_output(data_type, "Grid").structure_type(StructureType::Grid);
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.node().custom1);

  const float3 bounds_min = params.extract_input<float3>("Min");
  const float3 bounds_max = params.extract_input<float3>("Max");

  const int3 resolution = int3(params.extract_input<int>("Resolution X"),
                               params.extract_input<int>("Resolution Y"),
                               params.extract_input<int>("Resolution Z"));

  if (resolution.x < 2 || resolution.y < 2 || resolution.z < 2) {
    params.error_message_add(NodeWarningType::Error, TIP_("Resolution must be greater than 1"));
    params.set_default_remaining_outputs();
    return;
  }

  if (bounds_min.x == bounds_max.x || bounds_min.y == bounds_max.y || bounds_min.z == bounds_max.z)
  {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Bounding box volume must be greater than 0"));
    params.set_default_remaining_outputs();
    return;
  }

  const double3 scale_fac = double3(bounds_max - bounds_min) / double3(resolution - 1);
  if (!BKE_volume_voxel_size_valid(float3(scale_fac))) {
    params.error_message_add(NodeWarningType::Warning,
                             TIP_("Volume scale is lower than permitted by OpenVDB"));
    params.set_default_remaining_outputs();
    return;
  }

  bke::attribute_math::convert_to_static_type(
      *bke::socket_type_to_geo_nodes_base_cpp_type(data_type), [&](auto type_tag) {
        using ValueT = decltype(type_tag);
        using type_traits = typename bke::VolumeGridTraits<ValueT>;
        using TreeType = typename type_traits::TreeType;
        using GridType = openvdb::Grid<TreeType>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          const typename type_traits::BlenderType background =
              params.extract_input<typename type_traits::BlenderType>("Background");

          Field<typename type_traits::BlenderType> input_field =
              params.extract_input<Field<typename type_traits::BlenderType>>("Value");

          /* Evaluate input field on a 3D grid. */
          blender::nodes::Grid3DFieldContext context(resolution, bounds_min, bounds_max);
          FieldEvaluator evaluator(context, context.points_num());
          Array<typename type_traits::BlenderType> values(context.points_num());
          evaluator.add_with_destination(std::move(input_field), values.as_mutable_span());
          evaluator.evaluate();

          /* Store resulting values in openvdb grid. */
          Array<typename type_traits::PrimitiveType> openvdb_values(values.size());
          for (int64_t i = 0; i < values.size(); i++) {
            openvdb_values[i] = type_traits::to_openvdb(values[i]);
          }

          auto openvdb_grid = GridType::create(type_traits::to_openvdb(background));

          using DenseType = openvdb::tools::Dense<typename type_traits::PrimitiveType,
                                                  openvdb::tools::LayoutZYX>;
          DenseType dense_grid{
              openvdb::math::CoordBBox({0, 0, 0},
                                       {resolution.x - 1, resolution.y - 1, resolution.z - 1}),
              openvdb_values.data()};
          /* Force all voxels to be active. OpenVDB only stores non-background values,
           * so use extreme tolerance values to ensure all field values are considered "different".
           */
          if constexpr (std::is_same_v<typename type_traits::BlenderType, float>) {
            openvdb::tools::copyFromDense(
                dense_grid, *openvdb_grid, std::numeric_limits<float>::lowest());
          }
          else if constexpr (std::is_same_v<typename type_traits::BlenderType, int>) {
            openvdb::tools::copyFromDense(
                dense_grid, *openvdb_grid, std::numeric_limits<int>::lowest());
          }
          else if constexpr (std::is_same_v<typename type_traits::BlenderType, bool>) {
            openvdb::tools::copyFromDense(dense_grid, *openvdb_grid, false);
            /* Boolean grids need manual activation since there are only two possible values. */
            openvdb_grid->tree().sparseFill(
                openvdb::math::CoordBBox({0, 0, 0},
                                         {resolution.x - 1, resolution.y - 1, resolution.z - 1}),
                openvdb_grid->background(),
                /*active=*/true);
          }
          else if constexpr (std::is_same_v<typename type_traits::BlenderType, float3>) {
            openvdb::tools::copyFromDense(
                dense_grid, *openvdb_grid, openvdb::Vec3f(std::numeric_limits<float>::lowest()));
          }
          openvdb_grid->transform().postScale(
              openvdb::math::Vec3d(scale_fac.x, scale_fac.y, scale_fac.z));
          openvdb_grid->transform().postTranslate(
              openvdb::math::Vec3d(bounds_min.x, bounds_min.y, bounds_min.z));

          bke::VolumeGrid<ValueT> volume_grid(std::move(openvdb_grid));
          params.set_output("Grid", std::move(volume_grid));
        }
      });
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Node socket data type",
                    rna_enum_node_socket_data_type_items,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCubeGrid");
  ntype.ui_name = "Cube Grid";
  ntype.ui_description = "Create a new grid with the values for each voxel evaluated from a field";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_cube_grid_cc
