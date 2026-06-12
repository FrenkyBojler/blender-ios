/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"

#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_geometry_nodes_values.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_mesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  b.add_input<decl::Vector>("Positions"_ustr).structure_type(StructureType::List).hide_value();
  b.add_input<decl::Int>("Edges"_ustr).structure_type(StructureType::List).hide_value();
  b.add_input<decl::Int>("Faces"_ustr).structure_type(StructureType::List).hide_value();

  b.add_output<decl::Geometry>("Mesh"_ustr);
}

static int get_num_corners_from_faces_list(const GListPtr &faces_list,
                                           MutableSpan<int> face_offsets)
{
  if (!faces_list) {
    return 0;
  }
  if (!faces_list->cpp_type().is<bke::SocketValueVariant>()) {
    return 0;
  }
  BLI_assert(faces_list->size() + 1 == face_offsets.size());
  int num_corners = 0;
  face_offsets[0] = 0;
  const auto values = faces_list->typed<bke::SocketValueVariant>().values();
  if (const auto *span_values = std::get_if<Span<bke::SocketValueVariant>>(&values)) {
    for (const int i : span_values->index_range()) {
      const bke::SocketValueVariant &value = (*span_values)[i];
      if (!value.is_list()) {
        return 0;
      }
      const GListPtr face_list = value.get<GListPtr>();
      if (!face_list->cpp_type().is<int>()) {
        return 0;
      }
      num_corners += face_list->size();
      face_offsets[i + 1] = num_corners;
    }
  }
  return num_corners;
}

static bool get_edges_from_edges_list(GeoNodeExecParams &params,
                                      const GListPtr &edges_list,
                                      const IndexRange verts,
                                      MutableSpan<int2> edges)
{
  if (!edges_list) {
    return true;
  }
  if (!edges_list->cpp_type().is<bke::SocketValueVariant>()) {
    params.error_message_add(NodeWarningType::Error, "Edges must be a list of integer lists");
    return false;
  }
  const auto values = edges_list->typed<bke::SocketValueVariant>().values();
  if (const auto *span_values = std::get_if<Span<bke::SocketValueVariant>>(&values)) {
    for (const int i : span_values->index_range()) {
      const bke::SocketValueVariant &value = (*span_values)[i];
      if (!value.is_list()) {
        params.error_message_add(NodeWarningType::Error, "Edges must be a list of integer lists");
        return false;
      }
      const GListPtr edge_list = value.get<GListPtr>();
      if (!edge_list->cpp_type().is<int>() || edge_list->size() != 2) {
        params.error_message_add(NodeWarningType::Error,
                                 "Edge must be a list of integers with exactly 2 elements");
        return false;
      }
      edge_list->varray().materialize(&edges[i]);
      if (!verts.contains(edges[i][0]) || !verts.contains(edges[i][1])) {
        params.error_message_add(NodeWarningType::Error, "Edge vertex index out of bounds");
        return false;
      }
    }
  }

  return true;
}

static bool get_corners_from_face_list(GeoNodeExecParams &params,
                                       const GListPtr &faces_list,
                                       const IndexRange verts,
                                       const OffsetIndices<int> faces,
                                       MutableSpan<int> corners)
{
  if (!faces_list) {
    return true;
  }
  if (!faces_list->cpp_type().is<bke::SocketValueVariant>()) {
    params.error_message_add(NodeWarningType::Error, "Faces must be a list of integer lists");
    return false;
  }
  const auto values = faces_list->typed<bke::SocketValueVariant>().values();
  if (const auto *span_values = std::get_if<Span<bke::SocketValueVariant>>(&values)) {
    for (const int i : span_values->index_range()) {
      const bke::SocketValueVariant &value = (*span_values)[i];
      if (!value.is_list()) {
        params.error_message_add(NodeWarningType::Error, "Faces must be a list of integer lists");
        return false;
      }
      const GListPtr face_list = value.get<GListPtr>();
      if (!face_list->cpp_type().is<int>()) {
        params.error_message_add(NodeWarningType::Error, "Face must be a list of integers");
        return false;
      }
      if (face_list->size() < 1) {
        params.error_message_add(NodeWarningType::Error,
                                 "Face must be a list of at least 3 integers");
        return false;
      }
      const IndexRange face = faces[i];
      BLI_assert(face_list->size() == face.size());
      /* TODO: Could use face_list->varray().materialize(corners.slice(face));
       * without the error handling. */
      const auto corner_values = face_list->typed<int>().values();
      if (const auto *span_corner_values = std::get_if<Span<int>>(&corner_values)) {
        for (const int i : span_corner_values->index_range()) {
          const int corner = face[i];
          corners[corner] = (*span_corner_values)[i];
          if (!verts.contains(corners[corner])) {
            params.error_message_add(NodeWarningType::Error, "Corner index out of bounds");
            return false;
          }
        }
      }
    }
  }
  return true;
}

static Mesh *create_mesh_from_lists(GeoNodeExecParams &params,
                                    const GListPtr &positions_list,
                                    const GListPtr &edges_list,
                                    const GListPtr &faces_list)
{
  if (!positions_list) {
    return nullptr;
  }
  if (positions_list->size() < 1) {
    params.error_message_add(NodeWarningType::Error,
                             "Positions must be a list of at least one element");
    return nullptr;
  }
  if (!positions_list->cpp_type().is<float3>()) {
    params.error_message_add(NodeWarningType::Error, "Positions must be a list of vectors");
    return nullptr;
  }
  const int verts_num = positions_list->size();
  const IndexRange verts(verts_num);

  const int edges_num = edges_list ? edges_list->size() : 0;
  int faces_num = faces_list ? faces_list->size() : 0;

  Array<int> face_offsets(faces_num + 1);
  int corners_num = get_num_corners_from_faces_list(faces_list, face_offsets);

  Mesh *mesh = BKE_mesh_new_nomain(verts_num, edges_num, faces_num, corners_num);

  positions_list->typed<float3>().varray().materialize(mesh->vert_positions_for_write());

  if (edges_num > 0) {
    if (!get_edges_from_edges_list(params, edges_list, verts, mesh->edges_for_write())) {
      BKE_id_free_ex(nullptr, mesh, LIB_ID_FREE_NO_MAIN, false);
      return nullptr;
    }
  }

  if (faces_num > 0) {
    mesh->face_offsets_for_write().copy_from(face_offsets.as_span());
  }

  if (faces_num > 0) {
    if (!get_corners_from_face_list(
            params, faces_list, verts, mesh->faces(), mesh->corner_verts_for_write()))
    {
      BKE_id_free_ex(nullptr, mesh, LIB_ID_FREE_NO_MAIN, false);
      return nullptr;
    }
  }

  if (edges_num > 0) {
    bke::mesh_calc_edges(*mesh, true, false);
  }

  return mesh;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (params.output_is_required("Mesh"_ustr)) {
    const GListPtr positions_list = params.extract_input<GListPtr>("Positions"_ustr);
    const GListPtr edges_list = params.extract_input<GListPtr>("Edges"_ustr);
    const GListPtr faces_list = params.extract_input<GListPtr>("Faces"_ustr);
    Mesh *mesh = create_mesh_from_lists(params, positions_list, edges_list, faces_list);
    params.set_output("Mesh"_ustr, GeometrySet::from_mesh(mesh));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeMesh"_ustr);
  ntype.ui_name = "Mesh";
  ntype.ui_description = "Create a new mesh from a list of positions, edges, and faces";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  // ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_mesh_cc
