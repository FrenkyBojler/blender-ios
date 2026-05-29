/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "draw_sculpt.hh"

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "draw_attributes.hh"
#include "draw_context_private.hh"
#include "draw_view.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_customdata.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "BLI_math_matrix.hh"

#include "bmesh_class.hh"

#include "DRW_pbvh.hh"
#include "DRW_render.hh"

namespace blender::draw {

float3 SculptBatch::debug_color()
{
  static float3 colors[9] = {
      {1.0f, 0.2f, 0.2f},
      {0.2f, 1.0f, 0.2f},
      {0.2f, 0.2f, 1.0f},
      {1.0f, 1.0f, 0.2f},
      {0.2f, 1.0f, 1.0f},
      {1.0f, 0.2f, 1.0f},
      {1.0f, 0.7f, 0.2f},
      {0.2f, 1.0f, 0.7f},
      {0.7f, 0.2f, 1.0f},
  };

  return colors[debug_index % 9];
}

static Vector<SculptBatch> sculpt_batches_get_ex(const Object *ob,
                                                 const bool use_wire,
                                                 const Span<pbvh::AttributeRequest> attrs)
{
  /* pbvh::Tree should always exist for non-empty meshes, created by depsgraph eval. */
  bke::pbvh::Tree *pbvh = ob->runtime->sculpt_session ?
                              const_cast<bke::pbvh::Tree *>(bke::object::pbvh_get(*ob)) :
                              nullptr;
  if (!pbvh) {
    return {};
  }

  /* TODO(Miguel Pozo): Don't use global context. */
  const DRWContext *drwctx = DRW_context_get();
  RegionView3D *rv3d = drwctx->rv3d;
  const bool navigating = rv3d && (rv3d->rflag & RV3D_NAVIGATING);

  Paint *paint = nullptr;
  if (drwctx->evil_C != nullptr) {
    paint = BKE_paint_get_active_from_context(drwctx->evil_C);
  }

  /* TODO: take into account partial redraw for clipping planes. */
  /* Frustum planes to show only visible pbvh::Tree nodes. */
  std::array<float4, 6> draw_frustum_planes = View::default_get().frustum_planes_get();
  /* Transform clipping planes to object space. Transforming a plane with a
   * 4x4 matrix is done by multiplying with the transpose inverse.
   * The inverse cancels out here since we transform by inverse(obmat). */
  float4x4 tmat = math::transpose(ob->object_to_world());
  for (int i : IndexRange(draw_frustum_planes.size())) {
    draw_frustum_planes[i] = tmat * draw_frustum_planes[i];
  }

  /* Fast mode to show low poly multires while navigating. */
  bool fast_mode = false;
  if (paint && (paint->flags & PAINT_FAST_NAVIGATE)) {
    fast_mode = navigating;
  }

  /* Update draw buffers only for visible nodes while painting.
   * But do update them otherwise so navigating stays smooth. */
  bool update_only_visible = rv3d && !(rv3d->rflag & RV3D_PAINTING);
  if (paint && (paint->flags & PAINT_SCULPT_DELAY_UPDATES)) {
    update_only_visible = true;
  }

  bke::pbvh::update_normals_from_eval(*const_cast<Object *>(ob), *pbvh);

  pbvh::DrawCache &draw_data = pbvh::ensure_draw_data(pbvh->draw_data);

  IndexMaskMemory memory;
  const IndexMask visible_nodes = bke::pbvh::search_nodes(
      *pbvh, memory, [&](const bke::pbvh::Node &node) {
        return !BKE_pbvh_node_fully_hidden_get(node) &&
               bke::pbvh::node_frustum_contain_aabb(node, draw_frustum_planes);
      });

  const IndexMask nodes_to_update = update_only_visible ? visible_nodes :
                                                          bke::pbvh::all_leaf_nodes(*pbvh, memory);

  pbvh::ViewportRequest request{Vector<pbvh::AttributeRequest>(attrs), fast_mode};

  /* Try combined draw data for Vulkan draw call reduction (Grids PBVH only). */
  /* Split into flat and smooth layout groups to handle mixed layouts correctly. */
  if (!use_wire) {
    draw_data.ensure_combined_tris_draw_data(*ob, request, visible_nodes);

    Vector<SculptBatch> result;

    /* Process flat layout combined draw data. */
    pbvh::PBVHDrawData *flat_combined_data = draw_data.get_combined_draw_data_flat(request,
                                                                                   attrs[0]);
    if (flat_combined_data && flat_combined_data->vbo && flat_combined_data->ibo &&
        flat_combined_data->indirect_buf)
    {
      gpu::Batch *combined_batch = GPU_batch_create(
          GPU_PRIM_TRIS, nullptr, flat_combined_data->ibo.get());
      for (const pbvh::AttributeRequest &attr : attrs) {
        pbvh::PBVHDrawData *attr_data = draw_data.get_combined_draw_data_flat(request, attr);
        if (attr_data && attr_data->vbo) {
          GPU_batch_vertbuf_add(combined_batch, attr_data->vbo, false);
        }
      }
      result.resize((int)result.size() + 1);
      result[result.size() - 1] = {};
      result[result.size() - 1].batch = combined_batch;
      result[result.size() - 1].indirect_buf = flat_combined_data->indirect_buf.get();
      result[result.size() - 1].draw_count = (uint32_t)flat_combined_data->node_ranges.size();
      result[result.size() - 1].material_slot = 0;
      result[result.size() - 1].debug_index = (uint32_t)result.size() - 1;
    }

    /* Process smooth layout combined draw data. */
    pbvh::PBVHDrawData *smooth_combined_data = draw_data.get_combined_draw_data_smooth(request,
                                                                                       attrs[0]);
    if (smooth_combined_data && smooth_combined_data->vbo && smooth_combined_data->ibo &&
        smooth_combined_data->indirect_buf)
    {
      gpu::Batch *combined_batch = GPU_batch_create(
          GPU_PRIM_TRIS, nullptr, smooth_combined_data->ibo.get());
      for (const pbvh::AttributeRequest &attr : attrs) {
        pbvh::PBVHDrawData *attr_data = draw_data.get_combined_draw_data_smooth(request, attr);
        if (attr_data && attr_data->vbo) {
          GPU_batch_vertbuf_add(combined_batch, attr_data->vbo, false);
        }
      }
      result.resize((int)result.size() + 1);
      result[result.size() - 1] = {};
      result[result.size() - 1].batch = combined_batch;
      result[result.size() - 1].indirect_buf = smooth_combined_data->indirect_buf.get();
      result[result.size() - 1].draw_count = (uint32_t)smooth_combined_data->node_ranges.size();
      result[result.size() - 1].material_slot = 0;
      result[result.size() - 1].debug_index = (uint32_t)result.size() - 1;
    }

    if (!result.is_empty()) {
      return result;
    }
  }
  else {
    /* Wireframe combined draw. */
    pbvh::PBVHDrawData *combined_lines = draw_data.get_combined_lines_draw_data();
    if (!combined_lines || !combined_lines->vbo || !combined_lines->ibo) {
      draw_data.ensure_combined_lines_draw_data(*ob, request, visible_nodes);
      combined_lines = draw_data.get_combined_lines_draw_data();
    }

    if (combined_lines && combined_lines->vbo && combined_lines->ibo &&
        combined_lines->indirect_buf)
    {
      gpu::Batch *combined_batch = GPU_batch_create(
          GPU_PRIM_LINES, combined_lines->vbo, combined_lines->ibo.get());
      Vector<SculptBatch> result;
      result.resize(1);
      result[0] = {};
      result[0].batch = combined_batch;
      result[0].indirect_buf = combined_lines->indirect_buf.get();
      result[0].material_slot = 0;
      result[0].debug_index = 0;
      return result;
    }
  }

  /* Fallback to per-node batches. */
  Span<gpu::Batch *> batches;
  if (use_wire) {
    batches = draw_data.ensure_lines_batches(*ob, request, nodes_to_update);
  }
  else {
    batches = draw_data.ensure_tris_batches(*ob, request, nodes_to_update);
  }

  const Span<int> material_indices = draw_data.ensure_material_indices(*ob);

  const int max_material = std::max(0, BKE_object_material_count_eval(ob) - 1);
  Vector<SculptBatch> result_batches(visible_nodes.size());
  visible_nodes.foreach_index([&](const int i, const int pos) {
    result_batches[pos] = {};
    result_batches[pos].batch = batches[i];
    result_batches[pos].indirect_buf = nullptr;
    result_batches[pos].material_slot = material_indices.is_empty() ?
                                            0 :
                                            std::clamp(material_indices[i], 0, max_material);
    result_batches[pos].debug_index = pos;
  });

  BLI_assert(result_batches.size() == visible_nodes.size());

  return result_batches;
}

Vector<SculptBatch> sculpt_batches_get(const Object *ob, SculptBatchFeature features)
{
  Vector<pbvh::AttributeRequest, 16> attrs;

  attrs.append(pbvh::CustomRequest::Position);
  attrs.append(pbvh::CustomRequest::Normal);
  if (features & SCULPT_BATCH_MASK) {
    attrs.append(pbvh::CustomRequest::Mask);
  }
  if (features & SCULPT_BATCH_FACE_SET) {
    attrs.append(pbvh::CustomRequest::FaceSet);
  }

  const Mesh *mesh = BKE_object_get_original_mesh(ob);
  if (features & SCULPT_BATCH_VERTEX_COLOR) {
    if (const char *name = mesh->active_color_attribute) {
      attrs.append(pbvh::GenericRequest(name));
    }
  }

  if (features & SCULPT_BATCH_UV) {
    const StringRef uv_name = mesh->active_uv_map_name();
    if (!uv_name.is_empty()) {
      attrs.append(pbvh::GenericRequest(uv_name));
    }
  }

  return sculpt_batches_get_ex(ob, features & SCULPT_BATCH_WIREFRAME, attrs);
}

Vector<SculptBatch> sculpt_batches_per_material_get(const Object *ob,
                                                    Span<const GPUMaterial *> materials)
{
  BLI_assert(ob->type == OB_MESH);
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob);

  VectorSet<std::string> draw_attrs;
  DRW_MeshCDMask cd_needed;
  DRW_mesh_get_attributes(*ob, mesh, materials, &draw_attrs, &cd_needed);

  Vector<pbvh::AttributeRequest, 16> attrs;

  attrs.append(pbvh::CustomRequest::Position);
  attrs.append(pbvh::CustomRequest::Normal);

  for (const StringRef name : draw_attrs) {
    attrs.append(pbvh::GenericRequest(name));
  }

  for (const StringRef name : cd_needed.uv) {
    attrs.append(pbvh::GenericRequest(name));
  }

  return sculpt_batches_get_ex(ob, false, attrs);
}

}  // namespace blender::draw
