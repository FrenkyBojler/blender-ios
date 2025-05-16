/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_delaunay_2d.hh"

#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

#include "GEO_CDT_to_mesh.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_triangulate_points_cc {

enum class TriangulationMode : int8_t {
  Full = 0,
  Inside = 1,
  InsideWidthHoles = 2,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Points").supported_type({GeometryComponent::Type::PointCloud});
  b.add_input<decl::Int>("Face ID").field_on_all().hide_value().description(
      "An index used to group points into faces");
  b.add_output<decl::Geometry>("Mesh").propagate_all_instance_attributes();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(TriangulationMode::Full);
}

static Array<meshintersect::CDT_result<double>> triangulate_points(GeometrySet &geometry_set, const TriangulationMode mode)
{
  const PointCloud *pointcloud = geometry_set.get_pointcloud();
  const Span<float3> positions = pointcloud->positions();

  Array<double2> positions_2d(positions.size());
  threading::parallel_for(positions.index_range(), 8196, [&](const IndexRange range) {
    for (const int point : range) {
      positions_2d[point] = double2(positions[point].xy());
    }
  });

  meshintersect::CDT_input<double> input;
  input.need_ids = false;
  input.vert = std::move(positions_2d);
  input.edge 

  CDT_output_type output_type;
  switch (mode) {
    case TriangulationMode::Full:
      output_type = CDT_FULL;
      break;
    case TriangulationMode::Inside:
      output_type = CDT_INSIDE;
      break;
    case TriangulationMode::InsideWidthHoles:
      output_type = CDT_INSIDE_WITH_HOLES;
      break;
    default:
      BLI_assert_unreachable();
  }

  return {delaunay_2d_calc(input, output_type)};
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");

  const TriangulationMode mode = TriangulationMode(params.node().custom1);

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_pointcloud()) {
      const Array<meshintersect::CDT_result<double>> results = triangulate_points(geometry_set, mode);
      Mesh *mesh = geometry::cdts_to_mesh(results);
      geometry_set.replace_mesh(mesh);
    }
    geometry_set.keep_only_during_modify({GeometryComponent::Type::Mesh});
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static const EnumPropertyItem mode_items[] = {
      {int(TriangulationMode::Full), "FULL", 0, "Full", ""},
      {int(TriangulationMode::Inside), "INSIDE", 0, "Inside", ""},
      {int(TriangulationMode::InsideWidthHoles), "INSIDE_WITH_HOLES", 0, "Inside With Holes", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "mode",
                    "Mode",
                    "Mode for constrained delaunay triangulation",
                    mode_items,
                    NOD_inline_enum_accessors(custom1));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeTriangulatePoints");
  ntype.ui_name = "Triangulate Points";
  ntype.ui_description =
      "Generate a mesh from a set of points using a constrained delaunay triangulation";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype,
                                  "NodeGeometryTriangulatePoints",
                                  node_free_standard_storage,
                                  node_copy_standard_storage);
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_triangulate_points_cc
