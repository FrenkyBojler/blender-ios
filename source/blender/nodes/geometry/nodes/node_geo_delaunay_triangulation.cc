/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_index_mask.hh"

#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

#include "FN_field.hh"

#include "GEO_CDT_to_mesh.hh"
#include "GEO_mesh_selection.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

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
                       GeometryComponent::Type::PointCloud})
      .description(
          "The geometries that are used to constrain the triangulation using the points, edges, "
          "and faces");
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

// static meshintersect::CDT_result<double> do_single_cdt(meshintersect::CDT_input<double> &input,
//                                                        const TriangulationMode mode)
// {
//   CDT_output_type output_type;
//   switch (mode) {
//     case TriangulationMode::Full:
//       output_type = CDT_FULL;
//       break;
//     case TriangulationMode::Inside:
//       output_type = CDT_INSIDE;
//       break;
//     case TriangulationMode::InsideWidthHoles:
//       output_type = CDT_INSIDE_WITH_HOLES;
//       break;
//     default:
//       BLI_assert_unreachable();
//   }

//   return delaunay_2d_calc(input, output_type);
// }

static Array<meshintersect::CDT_result<double>> do_cdts(
    const Span<meshintersect::CDT_input<double>> inputs, const CDT_output_type output_type)
{
  Array<meshintersect::CDT_result<double>> outputs(inputs.size());
  /* TODO: Use better grain size. */
  threading::parallel_for(inputs.index_range(), 8, [&](const IndexRange range) {
    for (const int i : range) {
      outputs[i] = delaunay_2d_calc(inputs[i], output_type);
    }
  });

  return outputs;
}

// static Array<meshintersect::CDT_result<double>> triangulate_points(GeometrySet &geometry_set,
//                                                                    const TriangulationMode mode)
// {
//   const PointCloud *pointcloud = geometry_set.get_pointcloud();
//   const Span<float3> positions = pointcloud->positions();

//   Array<double2> positions_2d(positions.size());
//   threading::parallel_for(positions.index_range(), 8196, [&](const IndexRange range) {
//     for (const int point : range) {
//       positions_2d[point] = double2(positions[point].xy());
//     }
//   });

//   meshintersect::CDT_input<double> input;
//   input.need_ids = true;
//   input.vert = std::move(positions_2d);

//   return {do_single_cdt(input, mode)};
// }

// static Array<meshintersect::CDT_result<double>> do_mesh_cdt(const Mesh &mesh,
//                                                             const Field<int> &group_index,
//                                                             const CDT_output_type output_type)
// {
//   // const bke::GeometryFieldContext field_context{mesh, AttrDomain::Point};
//   // fn::FieldEvaluator evaluator{field_context, mesh.verts_num};
//   // evaluator.add(group_index);
//   // evaluator.evaluate();
//   // const VArray<int> group_ids = evaluator.get_evaluated<int>(0);

//   const Span<float3> positions = mesh.vert_positions();
//   Array<double2> positions_2d(positions.size());
//   threading::parallel_for(positions.index_range(), 8196, [&](const IndexRange range) {
//     for (const int point : range) {
//       positions_2d[point] = double2(positions[point].xy());
//     }
//   });

//   meshintersect::CDT_input<double> input;
//   input.need_ids = false;
//   input.vert = std::move(positions_2d);

//   /* Edges that are connected to a face are automatically added. So only add loose edges. */
//   if (mesh.loose_edges().count > 0) {
//     const Span<int2> edges = mesh.edges();
//     Array<std::pair<int, int>> input_edges(mesh.loose_edges().count);

//     IndexMaskMemory memory;
//     const IndexMask loose_edges = IndexMask::from_bits(mesh.loose_edges().is_loose_bits,
//     memory); loose_edges.foreach_index(GrainSize(4096), [&](const int index, const int pos) {
//       input_edges[pos] = {edges[index].x, edges[index].y};
//     });

//     input.edge = std::move(input_edges);
//   }

//   if (!mesh.faces().is_empty()) {
//     const OffsetIndices faces = mesh.faces();
//     const Span<int> corner_verts = mesh.corner_verts();
//     Array<Vector<int>> input_faces(faces.size());

//     threading::parallel_for(faces.index_range(), 4096, [&](const IndexRange range) {
//       for (const int i : range) {
//         const IndexRange face = faces[i];
//         input_faces[i] = corner_verts.slice(face);
//       }
//     });

//     input.face = std::move(input_faces);
//   }
//   else {
//     /* If there are no faces, treat everything as one face. */
//     Array<Vector<int>> input_faces(1);
//     input_faces[0].reinitialize(positions.size());
//     array_utils::fill_index_range(input_faces[0].as_mutable_span());
//     input.face = std::move(input_faces);
//   }

//   return {delaunay_2d_calc(input, output_type)};
// }

struct CDTGeometryInput {
  /* A span of all the 3d positions of this input geometry. */
  Span<float3> positions;
  Span<int2> edges;
  OffsetIndices<int> faces;
  Span<int> corner_verts;

  /* Group IDs used by this geometry. */
  VectorSet<int> group_ids;
  /* An index mask of points for each group ID in the geometry. */
  Vector<IndexMask> point_masks;
  /* An index mask of edges for each group ID in the geometry. */
  Vector<IndexMask> edge_masks;
  /* An index mask of faces for each group ID in the geometry. */
  Vector<IndexMask> face_masks;

  Vector<Map<int, int>> src_to_dst_point_maps;
};

static Vector<meshintersect::CDT_input<double>> construct_cdt_inputs(
    const GeometrySet &geometry_set, const Field<int> &group_index)
{
  IndexMaskMemory memory;

  Vector<CDTGeometryInput, 4> geometry_inputs;
  Vector<const GeometryComponent *> components = geometry_set.get_components();
  for (const GeometryComponent *component : components) {
    if (!ELEM(component->type(),
              bke::GeometryComponent::Type::PointCloud,
              bke::GeometryComponent::Type::Curve,
              bke::GeometryComponent::Type::GreasePencil, /* TODO! */
              bke::GeometryComponent::Type::Mesh))
    {
      continue;
    }
    bke::GeometryFieldContext field_context{*component, bke::AttrDomain::Point};
    const int point_num = component->attribute_domain_size(bke::AttrDomain::Point);
    FieldEvaluator field_evaluator{field_context, point_num};
    field_evaluator.add(group_index);
    field_evaluator.evaluate();

    CDTGeometryInput geometry_input;
    geometry_input.point_masks = IndexMask::from_group_ids(
        field_evaluator.get_evaluated<int>(0), memory, geometry_input.group_ids);
    const int num_group_ids = geometry_input.group_ids.size();

    switch (component->type()) {
      case bke::GeometryComponent::Type::PointCloud: {
        const PointCloud *pointcloud = geometry_set.get_pointcloud();
        geometry_input.positions = pointcloud->positions();
        break;
      }
      case bke::GeometryComponent::Type::Mesh: {
        const Mesh *mesh = geometry_set.get_mesh();
        geometry_input.positions = mesh->vert_positions();
        geometry_input.edges = mesh->edges();
        geometry_input.faces = mesh->faces();
        geometry_input.corner_verts = mesh->corner_verts();

        Vector<IndexMask> edge_masks(num_group_ids);
        Vector<IndexMask> face_masks(num_group_ids);
        for (const int group_index : IndexRange(num_group_ids)) {
          Array<bool> point_selection(mesh->verts_num);
          const IndexMask points = geometry_input.point_masks[group_index];
          points.to_bools(point_selection.as_mutable_span());

          edge_masks[group_index] = std::move(geometry::edge_selection_from_vert(
              mesh->edges(), point_selection.as_span(), memory));
          face_masks[group_index] = std::move(geometry::face_selection_from_vert(
              mesh->faces(), mesh->corner_verts(), point_selection.as_span(), memory));
        }
        geometry_input.edge_masks = std::move(edge_masks);
        geometry_input.face_masks = std::move(face_masks);

        geometry_input.src_to_dst_point_maps.reinitialize(num_group_ids);
        break;
      }
      default:
        break;
    }
    geometry_inputs.append_as(std::move(geometry_input));
  }

  VectorSet<int> all_group_ids;
  for (const CDTGeometryInput &geometry_input : geometry_inputs) {
    all_group_ids.add_multiple(geometry_input.group_ids);
  }

  /* Create one CDT input per group ID. */
  Vector<meshintersect::CDT_input<double>> cdt_inputs(all_group_ids.size());
  for (const int input_i : cdt_inputs.index_range()) {
    const int group_id = all_group_ids[input_i];

    Vector<int> point_counts, edge_counts, face_counts;
    Vector<int> geometries_with_edges, geometries_with_faces, geometries_with_edges_or_faces;
    Vector<CDTGeometryInput *> geometry_inputs_for_group;
    for (const int geometry_i : geometry_inputs.index_range()) {
      CDTGeometryInput &geometry_input = geometry_inputs[geometry_i];
      const int group_index = geometry_input.group_ids.index_of_try(group_id);
      if (group_index == -1) {
        continue;
      }
      point_counts.append(geometry_input.point_masks[group_index].size());
      if (!geometry_input.edge_masks.is_empty()) {
        edge_counts.append(geometry_input.edge_masks[group_index].size());
        geometries_with_edges.append(geometry_inputs_for_group.size());
        geometries_with_edges_or_faces.append(geometry_inputs_for_group.size());
      }
      if (!geometry_input.face_masks.is_empty()) {
        face_counts.append(geometry_input.face_masks[group_index].size());
        geometries_with_faces.append(geometry_inputs_for_group.size());
        geometries_with_edges_or_faces.append(geometry_inputs_for_group.size());
      }
      geometry_inputs_for_group.append(&geometry_input);
    }
    BLI_assert(!geometry_inputs_for_group.is_empty());

    /* Append one more count so we can accumulate the counts to offsets inplace. */
    point_counts.append(0);
    edge_counts.append(0);
    face_counts.append(0);
    OffsetIndices<int> dst_points_by_geometry = offset_indices::accumulate_counts_to_offsets(
        point_counts.as_mutable_span());
    OffsetIndices<int> dst_edges_by_geometry, dst_faces_by_geometry;
    if (!geometries_with_edges.is_empty()) {
      dst_edges_by_geometry = offset_indices::accumulate_counts_to_offsets(
          edge_counts.as_mutable_span());
    }
    if (!geometries_with_faces.is_empty()) {
      dst_faces_by_geometry = offset_indices::accumulate_counts_to_offsets(
          face_counts.as_mutable_span());
    }

    /* Compute src to dst maps for later lookups. */
    if (!geometries_with_edges_or_faces.is_empty()) {
      for (const int geometry_i : geometries_with_edges_or_faces) {
        CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
        const IndexRange dst_points_range = dst_points_by_geometry[geometry_i];

        const int group_index = geometry_input.group_ids.index_of(group_id);
        const IndexMask src_points = geometry_input.point_masks[group_index];
        Map<int, int> &src_to_dst_point_map = geometry_input.src_to_dst_point_maps[group_index];
        src_points.foreach_index([&](const int64_t src_point, const int64_t pos) {
          const int64_t dst_point = pos + dst_points_range.start();
          src_to_dst_point_map.add(src_point, dst_point);
        });
      }
    }

    const int total_verts = dst_points_by_geometry.total_size();
    const int total_edges = dst_edges_by_geometry.total_size();
    const int total_faces = dst_faces_by_geometry.total_size();
    BLI_assert(total_verts > 0);

    meshintersect::CDT_input<double> &cdt_input = cdt_inputs[input_i];
    cdt_input.vert.reinitialize(total_verts);
    cdt_input.edge.reinitialize(total_edges);
    cdt_input.face.reinitialize(total_faces);
    cdt_input.need_ids = true;

    MutableSpan<double2> positions_2d = cdt_input.vert.as_mutable_span();
    for (const int geometry_i : geometry_inputs_for_group.index_range()) {
      const CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
      const int group_index = geometry_input.group_ids.index_of(group_id);
      const IndexMask src_points = geometry_input.point_masks[group_index];
      const Span<float3> positions = geometry_input.positions;

      const IndexRange dst_points_range = dst_points_by_geometry[geometry_i];
      MutableSpan<double2> dst_positions = positions_2d.slice(dst_points_range);
      src_points.foreach_index(GrainSize(4096), [&](const int64_t src_point, const int64_t pos) {
        dst_positions[pos] = double2(positions[src_point].xy());
      });
    }

    MutableSpan<std::pair<int, int>> input_edges = cdt_input.edge.as_mutable_span();
    for (const int geometry_i : geometries_with_edges) {
      const CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
      const int group_index = geometry_input.group_ids.index_of(group_id);
      const IndexMask src_edges = geometry_input.edge_masks[group_index];
      const Map<int, int> &src_to_dst_point_map =
          geometry_input.src_to_dst_point_maps[group_index];
      const Span<int2> edges = geometry_input.edges;

      const IndexRange dst_edges_range = dst_edges_by_geometry[geometry_i];
      MutableSpan<std::pair<int, int>> dst_edges = input_edges.slice(dst_edges_range);
      src_edges.foreach_index(GrainSize(4096), [&](const int64_t src_edge, const int64_t pos) {
        const int2 edge = edges[src_edge];
        BLI_assert(src_to_dst_point_map.contains(edge[0]) &&
                   src_to_dst_point_map.contains(edge[1]));
        dst_edges[pos] = {src_to_dst_point_map.lookup(edge[0]),
                          src_to_dst_point_map.lookup(edge[1])};
      });
    }

    MutableSpan<Vector<int>> input_faces = cdt_input.face.as_mutable_span();
    for (const int geometry_i : geometries_with_faces) {
      const CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
      const int group_index = geometry_input.group_ids.index_of(group_id);
      const IndexMask src_faces = geometry_input.face_masks[group_index];
      const Map<int, int> &src_to_dst_point_map =
          geometry_input.src_to_dst_point_maps[group_index];
      const OffsetIndices<int> faces = geometry_input.faces;
      const Span<int> corner_verts = geometry_input.corner_verts;

      const IndexRange dst_faces_range = dst_faces_by_geometry[geometry_i];
      MutableSpan<Vector<int>> dst_faces = input_faces.slice(dst_faces_range);
      src_faces.foreach_index(GrainSize(1024), [&](const int64_t src_face, const int64_t pos) {
        Vector<int> dst_face(faces[src_face].size());
        const Span<int> indices = corner_verts.slice(faces[src_face]);
        for (const int point : dst_face.index_range()) {
          const int src_point = indices[point];
          BLI_assert(src_to_dst_point_map.contains(src_point));
          dst_face[point] = src_to_dst_point_map.lookup(src_point);
        }
        dst_faces[pos] = std::move(dst_face);
      });
    }
  }
  return cdt_inputs;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<int> group_index = params.extract_input<Field<int>>("Group ID");

  const TriangulationMode mode = TriangulationMode(params.node().custom1);
  const CDT_output_type output_type = get_cdt_output_type(mode);

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    Vector<meshintersect::CDT_input<double>> inputs = construct_cdt_inputs(geometry_set,
                                                                           group_index);
    Array<meshintersect::CDT_result<double>> results = do_cdts(inputs, output_type);
    Mesh *mesh = geometry::cdts_to_mesh(results.as_span());
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
      "Generate a triangulated mesh from a set of points in the X-Y plane. Adds edges and faces "
      "as triangulation constraints";
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
