/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_task.hh"

#include "BKE_attribute_math.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/Dense.h>
#endif

namespace blender::nodes::node_geo_evaluate_on_grid_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_input(data_type, "Grid")
      .hide_value()
      .structure_type(StructureType::Grid)
      .description("Input grid to use for topology and transform information");

  b.add_input(data_type, "Value")
      .supports_field()
      .description("Field value to evaluate at each grid point");

  b.add_output(data_type, "Grid")
      .structure_type(StructureType::Grid)
      .align_with_previous()
      .description("Output grid with evaluated field values");
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

  // Handle different data types using template dispatch
  bke::attribute_math::convert_to_static_type(
      *bke::socket_type_to_geo_nodes_base_cpp_type(data_type), [&](auto type_tag) {
        using ValueT = decltype(type_tag);
        using type_traits = typename bke::VolumeGridTraits<ValueT>;

        if constexpr (!std::is_same_v<typename type_traits::BlenderType, void>) {
          // Get input grid for topology and transform
          const auto input_grid = params.extract_input<bke::VolumeGrid<ValueT>>("Grid");
          if (!input_grid) {
            params.set_default_remaining_outputs();
            return;
          }

          bke::VolumeTreeAccessToken token;
          const auto &source_grid = input_grid.grid(token);

          // Get grid bounds and resolution from the source grid
          const openvdb::math::CoordBBox bbox = source_grid.evalActiveVoxelBoundingBox();
          if (bbox.empty()) {
            params.set_default_remaining_outputs();
            return;
          }

          const int3 resolution = int3(bbox.dim().x(), bbox.dim().y(), bbox.dim().z());
          const openvdb::math::Vec3d min_world = source_grid.transform().indexToWorld(bbox.min());
          const openvdb::math::Vec3d max_world = source_grid.transform().indexToWorld(bbox.max());
          const float3 bounds_min = float3(min_world.x(), min_world.y(), min_world.z());
          const float3 bounds_max = float3(max_world.x(), max_world.y(), max_world.z());

          Field<typename type_traits::BlenderType> input_field =
              params.extract_input<Field<typename type_traits::BlenderType>>("Value");

          /* Evaluate input field on the grid topology. */
          blender::nodes::Grid3DFieldContext context(resolution, bounds_min, bounds_max);
          FieldEvaluator evaluator(context, context.points_num());
          Array<typename type_traits::BlenderType> values(context.points_num());
          evaluator.add_with_destination(std::move(input_field), values.as_mutable_span());
          evaluator.evaluate();

          /* Store resulting values in openvdb grid. */
          using OutputTreeType = typename type_traits::TreeType;
          using OutputGridType = openvdb::Grid<OutputTreeType>;
          auto output_grid = OutputGridType::create(
              type_traits::to_openvdb(typename type_traits::BlenderType{}));
          output_grid->setTransform(source_grid.transform().copy());

          Array<typename type_traits::PrimitiveType> openvdb_values(values.size());
          for (int64_t i = 0; i < values.size(); i++) {
            openvdb_values[i] = type_traits::to_openvdb(values[i]);
          }

          using DenseType = openvdb::tools::Dense<typename type_traits::PrimitiveType,
                                                  openvdb::tools::LayoutZYX>;
          DenseType dense_grid{bbox, openvdb_values.data()};

          /* Force all voxels to be active. OpenVDB only stores non-background values,
           * so use extreme tolerance values to ensure all field values are considered "different".
           */
          if constexpr (std::is_same_v<typename type_traits::BlenderType, float>) {
            openvdb::tools::copyFromDense(
                dense_grid, *output_grid, std::numeric_limits<float>::lowest());
          }
          else if constexpr (std::is_same_v<typename type_traits::BlenderType, int>) {
            openvdb::tools::copyFromDense(
                dense_grid, *output_grid, std::numeric_limits<int>::lowest());
          }
          else if constexpr (std::is_same_v<typename type_traits::BlenderType, bool>) {
            openvdb::tools::copyFromDense(dense_grid, *output_grid, false);
            /* Boolean grids need manual activation since there are only two possible values. */
            output_grid->tree().sparseFill(bbox, output_grid->background(), /*active=*/true);
          }
          else if constexpr (std::is_same_v<typename type_traits::BlenderType, float3>) {
            openvdb::tools::copyFromDense(
                dense_grid, *output_grid, openvdb::Vec3f(std::numeric_limits<float>::lowest()));
          }

          bke::VolumeGrid<ValueT> volume_grid(std::move(output_grid));
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

  geo_node_type_base(&ntype, "GeometryNodeEvaluateOnGrid");
  ntype.ui_name = "Evaluate on Grid";
  ntype.ui_description = "Evaluate fields at grid points and output new grids with the results";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_evaluate_on_grid_cc
