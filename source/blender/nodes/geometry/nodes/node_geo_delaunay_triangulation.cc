/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_delaunay_2d.hh"
#include "BLI_index_mask.hh"

#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

#include "FN_field.hh"

#include "GEO_CDT_to_mesh.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_delaunay_triangulation_cc {

enum class TriangulationMode : int8_t {
  Full = 0,
  Inside = 1,
  InsideWidthHoles = 2,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::GreasePencil,
                       GeometryComponent::Type::PointCloud});
  b.add_input<decl::Int>("Group ID")
      .field_on_all()
      .hide_value()
      .description(
          "An index used to group points together. Triangulation is done separately for each "
          "group");
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

static CDT_output_type get_cdt_output_type(const TriangulationMode mode)
{
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
  return output_type;
}

static meshintersect::CDT_result<double> do_single_cdt(meshintersect::CDT_input<double> &input,
                                                       const TriangulationMode mode)
{
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

  return delaunay_2d_calc(input, output_type);
}

static Array<meshintersect::CDT_result<double>> triangulate_points(GeometrySet &geometry_set,
                                                                   const TriangulationMode mode)
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

  return {do_single_cdt(input, mode)};
}

static Array<meshintersect::CDT_result<double>> do_mesh_cdt(const Mesh &mesh,
                                                            const Field<int> &group_index,
                                                            const CDT_output_type output_type)
{
  // const bke::GeometryFieldContext field_context{mesh, AttrDomain::Point};
  // fn::FieldEvaluator evaluator{field_context, mesh.verts_num};
  // evaluator.add(group_index);
  // evaluator.evaluate();
  // const VArray<int> group_ids = evaluator.get_evaluated<int>(0);

  const Span<float3> positions = mesh.vert_positions();
  Array<double2> positions_2d(positions.size());
  threading::parallel_for(positions.index_range(), 8196, [&](const IndexRange range) {
    for (const int point : range) {
      positions_2d[point] = double2(positions[point].xy());
    }
  });

  meshintersect::CDT_input<double> input;
  input.need_ids = false;
  input.vert = std::move(positions_2d);

  /* Edges that are connected to a face are automatically added. So only add loose edges. */
  if (mesh.loose_edges().count > 0) {
    const Span<int2> edges = mesh.edges();
    Array<std::pair<int, int>> input_edges(mesh.loose_edges().count);

    IndexMaskMemory memory;
    const IndexMask loose_edges = IndexMask::from_bits(mesh.loose_edges().is_loose_bits, memory);
    loose_edges.foreach_index(GrainSize(4096), [&](const int index, const int pos) {
      input_edges[pos] = {edges[index].x, edges[index].y};
    });

    input.edge = std::move(input_edges);
  }

  if (!mesh.faces().is_empty()) {
    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    Array<Vector<int>> input_faces(faces.size());

    threading::parallel_for(faces.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        const IndexRange face = faces[i];
        input_faces[i] = corner_verts.slice(face);
      }
    });

    input.face = std::move(input_faces);
  }

  return {delaunay_2d_calc(input, output_type)};
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<int> group_index = params.extract_input<Field<int>>("Group ID");

  const TriangulationMode mode = TriangulationMode(params.node().custom1);
  const CDT_output_type output_type = get_cdt_output_type(mode);

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    Vector<meshintersect::CDT_result<double>> results;
    if (geometry_set.has_pointcloud()) {
      results.extend(triangulate_points(geometry_set, mode));
    }
    if (geometry_set.has_mesh()) {
      results.extend(do_mesh_cdt(*geometry_set.get_mesh(), group_index, output_type));
    }

    Mesh *mesh = geometry::cdts_to_mesh(results);
    geometry_set.replace_mesh(mesh);
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

  geo_node_type_base(&ntype, "GeometryNodeDelaunayTriangulation");
  ntype.ui_name = "Delaunay Triangulation";
  ntype.ui_description =
      "Generate a triangulated mesh from a set of points and edge/face constraints in the XY "
      "plane";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  blender::bke::node_type_size(ntype, 160, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;

  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_delaunay_triangulation_cc
