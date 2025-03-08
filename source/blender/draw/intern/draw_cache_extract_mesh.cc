/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Extraction of Mesh data into VBO to feed to GPU.
 */

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BLI_map.hh"
#include "BLI_task.hh"

#include "GPU_capabilities.hh"

#include "draw_cache_extract.hh"
#include "draw_subdivision.hh"

#include "mesh_extractors/extract_mesh.hh"

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
                                        MeshBatchCache &cache,
                                        MeshBufferCache &mbc,
                                        Span<IBOType> ibo_requests,
                                        Span<VBOType> vbo_requests,
                                        Object &object,
                                        Mesh &mesh,
                                        const bool is_editmode,
                                        const bool is_paint_mode,
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
                                              object.object_to_world(),
                                              do_final,
                                              do_uvedit,
                                              use_hide,
                                              scene.toolsettings);

  ensure_dependency_data(mr, ibo_requests, vbo_requests, mbc);

  mr.use_subsurf_fdots = mr.mesh && !mr.mesh->runtime->subsurf_face_dot_tags.is_empty();
  mr.use_final_mesh = do_final;
  mr.use_simplify_normals = (scene.r.mode & R_SIMPLIFY) && (scene.r.mode & R_SIMPLIFY_NORMALS);

  bool lines = false;
  bool attrs = false;

  Map<IBOType, gpu::IndexBuf *> ibos_to_create;
  Map<VBOType, gpu::VertBuf *> vbos_to_create;
  for (const IBOType request : ibo_requests) {
    buffers.ibos.lookup_or_add_cb(request, [&]() {
      lines |= ELEM(request, IBOType::Lines, IBOType::LinesLoose);
      gpu::IndexBuf *ibo = GPU_indexbuf_calloc();
      ibos_to_create.add_new(request, ibo);
      return std::unique_ptr<gpu::IndexBuf, IndexBufDeleter>(ibo);
    });
  }
  for (const VBOType request : vbo_requests) {
    buffers.vbos.lookup_or_add_cb(request, [&]() {
      attrs |= int8_t(request) >= int8_t(VBOType::Attr0) &&
               int8_t(request) <= int8_t(VBOType::Attr15);
      gpu::VertBuf *vbo = GPU_vertbuf_calloc();
      vbos_to_create.add_new(request, vbo);
      return std::unique_ptr<gpu::VertBuf, VertBufDeleter>(vbo);
    });
  }

  if (lines) {
    extract_lines(mr,
                  ibos_to_create.lookup_default(IBOType::Lines, nullptr),
                  ibos_to_create.lookup_default(IBOType::LinesLoose, nullptr),
                  cache.no_loose_wire);
  }

  if (attrs) {
    for (int8_t i = int8_t(VBOType::Attr0); i <= int8_t(VBOType::Attr15); i++) {
      if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType(i), nullptr)) {
        extract_attribute(mr, cache.attr_used.requests[i], *vbo);
      }
    }
  }

  threading::parallel_for_each(ibos_to_create, [&](const auto request) {
    switch (request.first) {
      case IBOType::Tris:
        extract_tris(mr, mesh_render_data_faces_sorted_ensure(mr, mbc), cache, request.second);
        break;
      case IBOType::Lines:
      case IBOType::LinesLoose:
        /* Handled as a special case since they may share the same buffer. */
        break;
      case IBOType::Points:
        extract_points(mr, request.second);
        break;
      case IBOType::FaceDots:
        extract_face_dots(mr, request.second);
        break;
      case IBOType::LinesPaintMask:
        extract_lines_paint_mask(mr, request.second);
        break;
      case IBOType::LinesAdjacency:
        extract_lines_adjacency(mr, request.second, cache.is_manifold);
        break;
      case IBOType::EditUVTris:
        extract_edituv_tris(mr, request.second);
        break;
      case IBOType::EditUVLines:
        extract_edituv_lines(mr, request.second);
        break;
      case IBOType::EditUVPoints:
        extract_edituv_points(mr, request.second);
        break;
      case IBOType::EditUVFaceDots:
        extract_edituv_face_dots(mr, request.second);
        break;
    }
  });

  const bool do_hq_normals = (scene.r.perf_flag & SCE_PERF_HQ_NORMALS) != 0 ||
                             GPU_use_hq_normals_workaround();

  threading::parallel_for_each(vbos_to_create, [&](const auto request) {
    switch (request.first) {
      case VBOType::Position:
        extract_positions(mr, request.second);
        break;
      case VBOType::CornerNormal:
        extract_normals(mr, do_hq_normals, request.second);
        break;
      case VBOType::EdgeFactor:
        extract_edge_factor(mr, request.second);
        break;
      case VBOType::VertexGroupWeight:
        extract_weights(mr, cache, request.second);
        break;
      case VBOType::UVs:
        extract_uv_maps(mr, cache, request.second);
        break;
      case VBOType::Tangents:
        extract_tangents(mr, cache, do_hq_normals, request.second);
        break;
      case VBOType::SculptData:
        extract_sculpt_data(mr, request.second);
        break;
      case VBOType::Orco:
        extract_orco(mr, request.second);
        break;
      case VBOType::EditData:
        extract_edit_data(mr, request.second);
        break;
      case VBOType::EditUVData:
        extract_edituv_data(mr, request.second);
        break;
      case VBOType::EditUVStretchArea:
        extract_edituv_stretch_area(mr, request.second, cache.tot_area, cache.tot_uv_area);
        break;
      case VBOType::EditUVStretchAngle:
        extract_edituv_stretch_angle(mr, request.second);
        break;
      case VBOType::MeshAnalysis:
        extract_mesh_analysis(mr, request.second);
        break;
      case VBOType::FaceDotPosition:
        extract_face_dots_position(mr, request.second);
        break;
      case VBOType::FaceDotNormal:
        extract_face_dot_normals(mr, do_hq_normals, request.second);
        break;
      case VBOType::FaceDotUV:
        extract_face_dots_uv(mr, request.second);
        break;
      case VBOType::FaceDotEditUVData:
        extract_face_dots_edituv_data(mr, request.second);
        break;
      case VBOType::SkinRoots:
        extract_skin_roots(mr, request.second);
        break;
      case VBOType::IndexVert:
        extract_vert_index(mr, request.second);
        break;
      case VBOType::IndexEdge:
        extract_edge_index(mr, request.second);
        break;
      case VBOType::IndexFace:
        extract_face_index(mr, request.second);
        break;
      case VBOType::IndexFaceDot:
        extract_face_dot_index(mr, request.second);
        break;
      case VBOType::Attr0:
      case VBOType::Attr1:
      case VBOType::Attr2:
      case VBOType::Attr3:
      case VBOType::Attr5:
      case VBOType::Attr6:
      case VBOType::Attr7:
      case VBOType::Attr8:
      case VBOType::Attr9:
      case VBOType::Attr10:
      case VBOType::Attr11:
      case VBOType::Attr12:
      case VBOType::Attr13:
      case VBOType::Attr14:
      case VBOType::Attr15:
        /* Handled as a special case since they are extracted in the same function. */
        break;
      case VBOType::AttrViewer:
        extract_attr_viewer(mr, request.second);
        break;
      case VBOType::VertexNormal:
        extract_vert_normals(mr, request.second);
        break;
    }
  });
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

  bool lines = false;
  bool attrs = false;

  Map<IBOType, gpu::IndexBuf *> ibos_to_create;
  Map<VBOType, gpu::VertBuf *> vbos_to_create;
  for (const IBOType request : ibo_requests) {
    buffers.ibos.lookup_or_add_cb(request, [&]() {
      lines |= ELEM(request, IBOType::Lines, IBOType::LinesLoose);
      gpu::IndexBuf *ibo = GPU_indexbuf_calloc();
      ibos_to_create.add_new(request, ibo);
      return std::unique_ptr<gpu::IndexBuf, IndexBufDeleter>(ibo);
    });
  }
  for (const VBOType request : vbo_requests) {
    buffers.vbos.lookup_or_add_cb(request, [&]() {
      attrs |= int8_t(request) >= int8_t(VBOType::Attr0) &&
               int8_t(request) <= int8_t(VBOType::Attr15);
      gpu::VertBuf *vbo = GPU_vertbuf_calloc();
      vbos_to_create.add_new(request, vbo);
      return std::unique_ptr<gpu::VertBuf, VertBufDeleter>(vbo);
    });
  }

  if (vbos_to_create.contains(VBOType::Position) || vbos_to_create.contains(VBOType::Orco)) {
    extract_positions_subdiv(subdiv_cache,
                             mr,
                             *vbos_to_create.lookup(VBOType::Position),
                             vbos_to_create.lookup_default(VBOType::Orco, nullptr));
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::CornerNormal, nullptr)) {
    /* The corner normals calculation uses positions and normals stored in the `pos` VBO. */
    extract_normals_subdiv(mr, subdiv_cache, *buffers.vbos.lookup(VBOType::Position), *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::EdgeFactor, nullptr)) {
    extract_edge_factor_subdiv(subdiv_cache, mr, *buffers.vbos.lookup(VBOType::Position), *vbo);
  }
  if (ibos_to_create.contains(IBOType::Lines) || ibos_to_create.contains(IBOType::LinesLoose)) {
    extract_lines_subdiv(subdiv_cache,
                         mr,
                         ibos_to_create.lookup_default(IBOType::Lines, nullptr),
                         ibos_to_create.lookup_default(IBOType::LinesLoose, nullptr),
                         cache.no_loose_wire);
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::Tris, nullptr)) {
    extract_tris_subdiv(subdiv_cache, cache, *ibo);
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::Points, nullptr)) {
    extract_points_subdiv(mr, subdiv_cache, *ibo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::EditData, nullptr)) {
    extract_edit_data_subdiv(mr, subdiv_cache, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::Tangents, nullptr)) {
    extract_tangents_subdiv(mr, subdiv_cache, cache, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::IndexVert, nullptr)) {
    extract_vert_index_subdiv(subdiv_cache, mr, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::IndexEdge, nullptr)) {
    extract_edge_index_subdiv(subdiv_cache, mr, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::IndexFace, nullptr)) {
    extract_face_index_subdiv(subdiv_cache, mr, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::VertexGroupWeight, nullptr)) {
    extract_weights_subdiv(mr, subdiv_cache, cache, *vbo);
  }
  if (vbos_to_create.contains(VBOType::FaceDotNormal) ||
      vbos_to_create.contains(VBOType::FaceDotPosition) ||
      ibos_to_create.contains(IBOType::FaceDots))
  {
    /* We use only one extractor for face dots, as the work is done in a single compute shader. */
    extract_face_dots_subdiv(subdiv_cache,
                             *vbos_to_create.lookup_default(VBOType::FaceDotPosition, nullptr),
                             vbos_to_create.lookup_default(VBOType::FaceDotNormal, nullptr),
                             *ibos_to_create.lookup_default(IBOType::FaceDots, nullptr));
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::LinesPaintMask, nullptr)) {
    extract_lines_paint_mask_subdiv(mr, subdiv_cache, *ibo);
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::LinesAdjacency, nullptr)) {
    extract_lines_adjacency_subdiv(subdiv_cache, *ibo, cache.is_manifold);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::SculptData, nullptr)) {
    extract_sculpt_data_subdiv(mr, subdiv_cache, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::UVs, nullptr)) {
    /* Make sure UVs are computed before edituv stuffs. */
    extract_uv_maps_subdiv(subdiv_cache, cache, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::EditUVStretchArea, nullptr)) {
    extract_edituv_stretch_area_subdiv(mr, subdiv_cache, *vbo, cache.tot_area, cache.tot_uv_area);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::EditUVStretchAngle, nullptr)) {
    extract_edituv_stretch_angle_subdiv(mr, subdiv_cache, cache, *vbo);
  }
  if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType::EditUVData, nullptr)) {
    extract_edituv_data_subdiv(mr, subdiv_cache, *vbo);
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::EditUVTris, nullptr)) {
    extract_edituv_tris_subdiv(mr, subdiv_cache, *ibo);
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::EditUVLines, nullptr)) {
    extract_edituv_lines_subdiv(mr, subdiv_cache, *ibo);
  }
  if (gpu::IndexBuf *ibo = ibos_to_create.lookup_default(IBOType::EditUVPoints, nullptr)) {
    extract_edituv_points_subdiv(mr, subdiv_cache, *ibo);
  }
  if (attrs) {
    for (int8_t i = int8_t(VBOType::Attr0); i <= int8_t(VBOType::Attr15); i++) {
      if (gpu::VertBuf *vbo = vbos_to_create.lookup_default(VBOType(i), nullptr)) {
        extract_attribute_subdiv(mr, subdiv_cache, cache.attr_used.requests[i], *vbo);
      }
    }
  }
}

/** \} */

}  // namespace blender::draw
