/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Extraction of Mesh data into VBO to feed to GPU.
 */

#include "BLI_task.hh"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BLI_task.h"

#include "GPU_capabilities.hh"

#include "GPU_index_buffer.hh"
#include "GPU_vertex_buffer.hh"
#include "draw_cache_extract.hh"
#include "draw_cache_inline.hh"
#include "draw_subdivision.hh"

#include "mesh_extractors/extract_mesh.hh"
#include <memory>
#include <utility>

// #define DEBUG_TIME

#ifdef DEBUG_TIME
#  include "BLI_time_utildefines.h"
#endif

namespace blender::draw {

static void ensure_dependency_data(MeshRenderData &mr,
                                   Span<IBOType> ibo_requests,
                                   Span<VBOType> vbo_requests,
                                   MeshBufferCache &cache)
{
  const bool request_face_normals = vbo_requests.contains(VBOType::CornerNormal) ||
                                    vbo_requests.contains(VBOType::FaceDotNormal) ||
                                    vbo_requests.contains(VBOType::EdgeFactor) ||
                                    vbo_requests.contains(VBOType::MeshAnalysis);
  const bool request_corner_normals = vbo_requests.contains(VBOType::CornerNormal);
  const bool force_corner_normals = vbo_requests.contains(VBOType::Tangents);

  if (request_face_normals) {
    mesh_render_data_update_face_normals(mr);
  }
  if ((request_corner_normals && mr.normals_domain == bke::MeshNormalDomain::Corner &&
       !mr.use_simplify_normals) ||
      force_corner_normals)
  {
    mesh_render_data_update_corner_normals(mr);
  }

  const bool calc_loose_geom = ibo_requests.contains(IBOType::Lines) ||
                               ibo_requests.contains(IBOType::LinesLoose) ||
                               ibo_requests.contains(IBOType::Points) ||
                               vbo_requests.contains(VBOType::Position) ||
                               vbo_requests.contains(VBOType::EditData) ||
                               vbo_requests.contains(VBOType::VertexNormal) ||
                               vbo_requests.contains(VBOType::IndexVert) ||
                               vbo_requests.contains(VBOType::IndexEdge) ||
                               vbo_requests.contains(VBOType::EdgeFactor);

  if (calc_loose_geom) {
    mesh_render_data_update_loose_geom(mr, cache);
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Extract Loop
 * \{ */

void mesh_buffer_cache_create_requested(const Scene &scene,
                                        TaskGraph &task_graph,
                                        MeshBatchCache &cache,
                                        MeshBufferCache &mbc,
                                        Span<IBOType> ibo_requests,
                                        Span<VBOType> vbo_requests,
                                        Object &object,
                                        Mesh &mesh,
                                        const bool is_editmode,
                                        const bool is_paint_mode,
                                        const float4x4 &object_to_world,
                                        const bool do_final,
                                        const bool do_uvedit,
                                        const bool use_hide)
{
  if (ibo_requests.is_empty() && vbo_requests.is_empty()) {
    return;
  }

#ifdef DEBUG_TIME
  SCOPED_TIMER(__func__);
#endif

  MeshBufferList &buffers = mbc.buff;

  MeshRenderData mr = mesh_render_data_create(object,
                                              mesh,
                                              is_editmode,
                                              is_paint_mode,
                                              object_to_world,
                                              do_final,
                                              do_uvedit,
                                              use_hide,
                                              scene.toolsettings);

  ensure_dependency_data(mr, ibo_requests, vbo_requests, mbc);

  mr.use_subsurf_fdots = mr.mesh && !mr.mesh->runtime->subsurf_face_dot_tags.is_empty();
  mr.use_final_mesh = do_final;
  mr.use_simplify_normals = (scene.r.mode & R_SIMPLIFY) && (scene.r.mode & R_SIMPLIFY_NORMALS);

  Vector<std::pair<IBOType, gpu::IndexBuf &>> ibos_to_calculate;
  Vector<std::pair<VBOType, gpu::VertBuf &>> vbos_to_calculate;
  for (const IBOType request : ibo_requests) {
    buffers.ibos.lookup_or_add_cb(request, [&]() {
      gpu::IndexBuf *ibo = GPU_indexbuf_calloc();
      ibos_to_calculate.append({request, *ibo});
      return std::unique_ptr<gpu::IndexBuf, IndexBufDeleter>(ibo);
    });
  }
  for (const VBOType request : vbo_requests) {
    buffers.vbos.lookup_or_add_cb(request, [&]() {
      gpu::VertBuf *vbo = GPU_vertbuf_calloc();
      vbos_to_calculate.append({request, *vbo});
      return std::unique_ptr<gpu::VertBuf, VertBufDeleter>(vbo);
    });
  }

  threading::parallel_for_each(ibos_to_calculate, [&](const auto request) {
    switch (request.first) {
      case IBOType::Tris: {
        const SortedFaceData &face_sorted = mesh_render_data_faces_sorted_ensure(mr, mbc);
        extract_tris(mr, face_sorted, cache, request.second);
        break;
      }
      case IBOType::Lines: {
        break;
      }
      case IBOType::LinesLoose: {
        break;
      }
      case IBOType::Points: {
        extract_points(mr, request.second);
        break;
      }
      case IBOType::FaceDots: {
        extract_face_dots(mr, request.second);
        break;
      }
      case IBOType::LinesPaintMask: {
        extract_lines_paint_mask(mr, request.second);
        break;
      }
      case IBOType::LinesAdjacency: {
        extract_lines_adjacency(mr, request.second, cache.is_manifold);
        break;
      }
      case IBOType::EditUVTris: {
        extract_edituv_tris(mr, request.second);
        break;
      }
      case IBOType::EditUVLines: {
        extract_edituv_lines(mr, request.second);
        break;
      }
      case IBOType::EditUVPoints: {
        extract_edituv_points(mr, request.second);
        break;
      }
      case IBOType::EditUVFaceDots: {
        extract_edituv_face_dots(mr, request.second);
        break;
      }
    }
  });

  threading::parallel_for_each(vbos_to_calculate, [&](const auto request) {
    switch (request.first) {
      case VBOType::Position: {
        extract_positions(mr, request.second);
        break;
      }
      case VBOType::CornerNormal: {
        const bool do_hq_normals = (scene.r.perf_flag & SCE_PERF_HQ_NORMALS) != 0 ||
                                   GPU_use_hq_normals_workaround();
        extract_normals(mr, do_hq_normals, request.second);
        break;
      }
      case VBOType::EdgeFactor: {
        extract_edge_factor(mr, request.second);
        break;
      }
      case VBOType::VertexGroupWeight: {
        extract_weights(mr, cache, request.second);
        break;
      }
      case VBOType::UVs: {
        extract_uv_maps(mr, cache, request.second);
        break;
      }
      case VBOType::Tangents: {
        const bool do_hq_normals = (scene.r.perf_flag & SCE_PERF_HQ_NORMALS) != 0 ||
                                   GPU_use_hq_normals_workaround();
        extract_tangents(mr, cache, do_hq_normals, request.second);
        break;
      }
      case VBOType::SculptData: {
        extract_sculpt_data(mr, request.second);
        break;
      }
      case VBOType::Orco: {
        extract_orco(mr, request.second);
        break;
      }
      case VBOType::EditData: {
        extract_edit_data(mr, request.second);
        break;
      }
      case VBOType::EditUVData: {
        extract_edituv_data(mr, request.second);
        break;
      }
      case VBOType::EditUVStretchArea: {
        extract_edituv_stretch_area(mr, request.second, cache.tot_area, cache.tot_uv_area);
        break;
      }
      case VBOType::EditUVStretchAngle: {
        extract_edituv_stretch_angle(mr, request.second);
        break;
      }
      case VBOType::MeshAnalysis: {
        extract_mesh_analysis(mr, request.second);
        break;
      }
      case VBOType::FaceDotPosition: {
        extract_face_dots_position(mr, request.second);
        break;
      }
      case VBOType::FaceDotNormal: {
        const bool do_hq_normals = (scene.r.perf_flag & SCE_PERF_HQ_NORMALS) != 0 ||
                                   GPU_use_hq_normals_workaround();
        extract_face_dot_normals(mr, do_hq_normals, request.second);
        break;
      }
      case VBOType::FaceDotUV: {
        extract_face_dots_uv(mr, request.second);
        break;
      }
      case VBOType::FaceDotEditUVData: {
        extract_face_dots_edituv_data(mr, request.second);
        break;
      }
      case VBOType::SkinRoots: {
        extract_skin_roots(mr, request.second);
        break;
      }
      case VBOType::IndexVert: {
        extract_vert_index(mr, request.second);
        break;
      }
      case VBOType::IndexEdge: {
        extract_edge_index(mr, request.second);
        break;
      }
      case VBOType::IndexFace: {
        extract_face_index(mr, request.second);
        break;
      }
      case VBOType::IndexFaceDot: {
        extract_face_dot_index(mr, request.second);
        break;
      }
      case VBOType::Attr0: {
        break;
      }
      case VBOType::Attr1: {
        break;
      }
      case VBOType::Attr2: {
        break;
      }
      case VBOType::Attr3: {
        break;
      }
      case VBOType::Attr5: {
        break;
      }
      case VBOType::Attr6: {
        break;
      }
      case VBOType::Attr7: {
        break;
      }
      case VBOType::Attr8: {
        break;
      }
      case VBOType::Attr9: {
        break;
      }
      case VBOType::Attr10: {
        break;
      }
      case VBOType::Attr11: {
        break;
      }
      case VBOType::Attr12: {
        break;
      }
      case VBOType::Attr13: {
        break;
      }
      case VBOType::Attr14: {
        break;
      }
      case VBOType::Attr15: {
        break;
      }
      case VBOType::AttrViewer: {
        extract_attr_viewer(mr, request.second);
        break;
      }
      case VBOType::VertexNormal: {
        extract_vert_normals(mr, request.second);
        break;
      }
    }
  });

  if (DRW_ibo_requested(buffers.ibo.lines) || DRW_ibo_requested(buffers.ibo.lines_loose)) {
    struct TaskData {
      MeshRenderData &mr;
      MeshBufferList &buffers;
      MeshBatchCache &cache;
    };
    TaskNode *task_node = BLI_task_graph_node_create(
        &task_graph,
        [](void *__restrict task_data) {
          const TaskData &data = *static_cast<TaskData *>(task_data);
          extract_lines(data.mr,
                        data.buffers.ibo.lines,
                        data.buffers.ibo.lines_loose,
                        data.cache.no_loose_wire);
        },
        new TaskData{mr, buffers, cache},
        [](void *task_data) { delete static_cast<TaskData *>(task_data); });
  }
  if (attrs_requested) {
    struct TaskData {
      MeshRenderData &mr;
      MeshBufferList &buffers;
      MeshBatchCache &cache;
    };
    TaskNode *task_node = BLI_task_graph_node_create(
        &task_graph,
        [](void *__restrict task_data) {
          const TaskData &data = *static_cast<TaskData *>(task_data);
          extract_attributes(
              mr, {data.cache.attr_used.requests, GPU_MAX_ATTR}, {request.second, GPU_MAX_ATTR});
        },
        new TaskData{*mr, buffers, cache},
        [](void *task_data) { delete static_cast<TaskData *>(task_data); });
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Subdivision Extract Loop
 * \{ */

void mesh_buffer_cache_create_requested_subdiv(MeshBatchCache &cache,
                                               MeshBufferCache &mbc,
                                               Span<IBOType> ibo_requests,
                                               Span<VBOType> vbo_requests,
                                               DRWSubdivCache &subdiv_cache,
                                               MeshRenderData &mr)
{
  if (ibo_requests.is_empty() && vbo_requests.is_empty()) {
    return;
  }
  MeshBufferList &buffers = mbc.buff;

  mesh_render_data_update_corner_normals(mr);
  mesh_render_data_update_loose_geom(mr, mbc);
  DRW_subdivide_loose_geom(subdiv_cache, mbc);

  if (vbo_requests.contains(VBOType::Position) || vbo_requests.contains(VBOType::Orco)) {
    extract_positions_subdiv(subdiv_cache, mr, *buffers.vbo.pos, buffers.vbo.orco);
  }
  if (DRW_vbo_requested(buffers.vbo.nor)) {
    /* The corner normals calculation uses positions and normals stored in the `pos` VBO. */
    extract_normals_subdiv(mr, subdiv_cache, *buffers.vbo.pos, *buffers.vbo.nor);
  }
  if (DRW_vbo_requested(buffers.vbo.edge_fac)) {
    extract_edge_factor_subdiv(subdiv_cache, mr, *buffers.vbo.pos, *buffers.vbo.edge_fac);
  }
  if (DRW_ibo_requested(buffers.ibo.lines) || DRW_ibo_requested(buffers.ibo.lines_loose)) {
    extract_lines_subdiv(
        subdiv_cache, mr, buffers.ibo.lines, buffers.ibo.lines_loose, cache.no_loose_wire);
  }
  if (DRW_ibo_requested(buffers.ibo.tris)) {
    extract_tris_subdiv(subdiv_cache, cache, *buffers.ibo.tris);
  }
  if (DRW_ibo_requested(buffers.ibo.points)) {
    extract_points_subdiv(mr, subdiv_cache, *buffers.ibo.points);
  }
  if (DRW_vbo_requested(buffers.vbo.edit_data)) {
    extract_edit_data_subdiv(mr, subdiv_cache, *buffers.vbo.edit_data);
  }
  if (DRW_vbo_requested(buffers.vbo.tan)) {
    extract_tangents_subdiv(mr, subdiv_cache, cache, *buffers.vbo.tan);
  }
  if (DRW_vbo_requested(buffers.vbo.vert_idx)) {
    extract_vert_index_subdiv(subdiv_cache, mr, *buffers.vbo.vert_idx);
  }
  if (DRW_vbo_requested(buffers.vbo.edge_idx)) {
    extract_edge_index_subdiv(subdiv_cache, mr, *buffers.vbo.edge_idx);
  }
  if (DRW_vbo_requested(buffers.vbo.face_idx)) {
    extract_face_index_subdiv(subdiv_cache, mr, *buffers.vbo.face_idx);
  }
  if (DRW_vbo_requested(buffers.vbo.weights)) {
    extract_weights_subdiv(mr, subdiv_cache, cache, *buffers.vbo.weights);
  }
  if (DRW_vbo_requested(buffers.vbo.fdots_nor) || DRW_vbo_requested(buffers.vbo.fdots_pos) ||
      DRW_ibo_requested(buffers.ibo.fdots))
  {
    /* We use only one extractor for face dots, as the work is done in a single compute shader. */
    extract_face_dots_subdiv(
        subdiv_cache, *buffers.vbo.fdots_pos, buffers.vbo.fdots_nor, *buffers.ibo.fdots);
  }
  if (DRW_ibo_requested(buffers.ibo.lines_paint_mask)) {
    extract_lines_paint_mask_subdiv(mr, subdiv_cache, *buffers.ibo.lines_paint_mask);
  }
  if (DRW_ibo_requested(buffers.ibo.lines_adjacency)) {
    extract_lines_adjacency_subdiv(subdiv_cache, *buffers.ibo.lines_adjacency, cache.is_manifold);
  }
  if (DRW_vbo_requested(buffers.vbo.sculpt_data)) {
    extract_sculpt_data_subdiv(mr, subdiv_cache, *buffers.vbo.sculpt_data);
  }
  if (DRW_vbo_requested(buffers.vbo.uv)) {
    /* Make sure UVs are computed before edituv stuffs. */
    extract_uv_maps_subdiv(subdiv_cache, cache, *buffers.vbo.uv);
  }
  if (DRW_vbo_requested(buffers.vbo.edituv_stretch_area)) {
    extract_edituv_stretch_area_subdiv(
        mr, subdiv_cache, *buffers.vbo.edituv_stretch_area, cache.tot_area, cache.tot_uv_area);
  }
  if (DRW_vbo_requested(buffers.vbo.edituv_stretch_area)) {
    extract_edituv_stretch_angle_subdiv(
        mr, subdiv_cache, cache, *buffers.vbo.edituv_stretch_angle);
  }
  if (DRW_vbo_requested(buffers.vbo.edituv_data)) {
    extract_edituv_data_subdiv(mr, subdiv_cache, *buffers.vbo.edituv_data);
  }
  if (DRW_ibo_requested(buffers.ibo.edituv_tris)) {
    extract_edituv_tris_subdiv(mr, subdiv_cache, *buffers.ibo.edituv_tris);
  }
  if (DRW_ibo_requested(buffers.ibo.edituv_lines)) {
    extract_edituv_lines_subdiv(mr, subdiv_cache, *buffers.ibo.edituv_lines);
  }
  if (DRW_ibo_requested(buffers.ibo.edituv_points)) {
    extract_edituv_points_subdiv(mr, subdiv_cache, *buffers.ibo.edituv_points);
  }
  if (attrs_requested) {
    extract_attributes_subdiv(mr,
                              subdiv_cache,
                              {cache.attr_used.requests, GPU_MAX_ATTR},
                              {buffers.vbo.attr, GPU_MAX_ATTR});
  }
}

/** \} */

}  // namespace blender::draw
