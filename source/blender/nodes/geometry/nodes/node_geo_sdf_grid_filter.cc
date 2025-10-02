/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

#include "BKE_volume_grid.hh"

#ifdef WITH_OPENVDB
#  include "openvdb/tools/LevelSetFilter.h"
#endif

namespace blender::nodes::node_geo_sdf_grid_filter_cc {

enum class FilterType : int8_t {
  Fillet = 0,
  Laplacian = 1,
  Mean = 2,
  MeanCurvature = 3,
  Median = 4,
  Offset = 5,
};

static const EnumPropertyItem filter_type_items[] = {
    {int(FilterType::Fillet),
     "FILLET",
     0,
     "Fillet",
     "Rounds off concave internal corners to create smoother transitions. Only affects areas "
     "with negative principal curvature"},
    {int(FilterType::Laplacian),
     "LAPLACIAN",
     0,
     "Laplacian",
     "Laplacian flow smoothing. For true SDFs, this is equivalent to mean curvature flow but "
     "computationally cheaper. Ideal when combined with SDF normalization/rebuild"},
    {int(FilterType::Mean),
     "MEAN",
     0,
     "Mean",
     "Box filter smoothing for level sets. Fast separable averaging filter for general "
     "smoothing of the distance field"},
    {int(FilterType::MeanCurvature),
     "MEAN_CURVATURE",
     0,
     "Mean Curvature",
     "Mean curvature flow smoothing. Evolves the surface based on its mean curvature, "
     "naturally smoothing high-curvature regions more than flat areas"},
    {int(FilterType::Median),
     "MEDIAN",
     0,
     "Median",
     "Median filter for level sets. Reduces noise while preserving sharp features and edges in "
     "the distance field"},
    {int(FilterType::Offset),
     "OFFSET",
     0,
     "Offset",
     "Dilate or erode the SDF surface by moving it in/out by a world-space distance. Properly "
     "maintains the signed distance field property"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Float>("SDF Grid").hide_value().structure_type(StructureType::Grid);
  b.add_output<decl::Float>("SDF Grid").structure_type(StructureType::Grid).align_with_previous();
  b.add_input<decl::Menu>("Type")
      .static_items(filter_type_items)
      .default_value(FilterType::MeanCurvature)
      .optional_label()
      .description("Level set filtering algorithm to apply");
  b.add_input<decl::Float>("Distance")
      .default_value(0.1f)
      .description("World-space distance to offset the SDF surface")
      .usage_by_menu("Type", int(FilterType::Offset));
  b.add_input<decl::Int>("Iterations")
      .default_value(1)
      .min(1)
      .description("Repeatedly apply the filter to the grid this many times")
      .usage_by_menu("Type",
                     {int(FilterType::MeanCurvature),
                      int(FilterType::Mean),
                      int(FilterType::Median),
                      int(FilterType::Fillet),
                      int(FilterType::Laplacian)});
  b.add_input<decl::Int>("Width")
      .default_value(1)
      .min(1)
      .description(
          "Filter kernel radius for spatial filters (Mean, Median, Gaussian) or fillet radius "
          "in voxels")
      .usage_by_menu("Type", {int(FilterType::Mean), int(FilterType::Median)});
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  if (!USER_EXPERIMENTAL_TEST(&U, use_new_volume_nodes)) {
    return;
  }
  if (params.other_socket().type == SOCK_FLOAT) {
    params.add_item(IFACE_("Grid"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeSDFGridFilter");
      params.update_and_connect_available_socket(node, "Grid");
    });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const FilterType filter_type = params.get_input<FilterType>("Type");
  auto grid = params.extract_input<bke::VolumeGrid<float>>("SDF Grid");
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }

  bke::VolumeTreeAccessToken tree_token;
  openvdb::FloatGrid &vdb_grid = grid.grid_for_write(tree_token);

  /* These filters are meant for signed distance fields to results may vary wildly
  if being applied to other grid types.  */
  if (vdb_grid.getGridClass() != openvdb::GRID_LEVEL_SET) {
    params.error_message_add(
        NodeWarningType::Warning,
        "Input grid is not marked as a level set. Results may be unpredictable");
  }

  openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(vdb_grid);

  if (filter_type == FilterType::Offset) {
    const float distance = params.extract_input<float>("Distance");
    filter.offset(distance);
  }
  else {
    const int iterations = std::max(params.extract_input<int>("Iterations"), 1);
    const int width = std::max(params.extract_input<int>("Width"), 1);

    for (int i = 0; i < iterations; i++) {
      switch (filter_type) {
        case FilterType::Laplacian:
          filter.laplacian();
          break;
        case FilterType::MeanCurvature:
          filter.meanCurvature();
          break;
        case FilterType::Median:
          filter.median(width);
          break;
        case FilterType::Mean:
          filter.mean(width);
          break;
        case FilterType::Fillet:
          filter.fillet();
          break;
        case FilterType::Offset:
          break;
      }
    }
  }

  params.set_output("SDF Grid", std::move(grid));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSDFGridFilter");
  ntype.ui_name = "SDF Grid Filter";
  ntype.ui_description =
      "Apply level set filtering operations to signed distance fields. Includes curvature-based "
      "smoothing and spatial filters optimized for preserving distance field properties";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sdf_grid_filter_cc
