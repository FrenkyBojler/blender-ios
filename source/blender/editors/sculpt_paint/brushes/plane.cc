/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * "Plane" related brushes, all three of these perform a similar displacement with an optional
 * additional filtering step.
 */

#include "editors/sculpt_paint/brushes/types.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh_brush_common.hh"
#include "editors/sculpt_paint/sculpt_automask.hh"
#include "editors/sculpt_paint/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint {

inline namespace plane_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float3> local_positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

static void calc_local_positions(const float4x4& mat,
  const Span<int> verts,
  const Span<float3> positions,
  const MutableSpan<float3> local_positions)
{
  for (const int i : verts.index_range()) {
    local_positions[i] = math::transform_point(mat, positions[verts[i]]);
  }
}

static void calc_local_positions(const float4x4& mat,
  const Span<float3> positions,
  const MutableSpan<float3> local_positions)
{
  for (const int i : positions.index_range()) {
    local_positions[i] = math::transform_point(mat, positions[i]);
  }
}

static void calc_distances(const float depth,
  const float height,
  const MutableSpan<float3> local_positions,
  const MutableSpan<float> distances)
{
  if (depth != 0.0f)
  {
    const float depth_rcp = math::rcp(depth);

    for (const int i : local_positions.index_range())
    {
      const float3 position = local_positions[i];
      if (position.z < 0.0f) {
        distances[i] = math::length(float3(position.x, position.y, position.z * depth_rcp));
      }
    }
  }
  else {
    for (const int i : local_positions.index_range())
    {
      if (local_positions[i].z < 0.0f) {
        distances[i] = 1.0f;
      }
    }
  }

  if (height != 0.0f)
  {
    const float height_rcp = math::rcp(height);

    for (const int i : local_positions.index_range())
    {
      const float3 position = local_positions[i];
      if (position.z >= 0.0f) {
        distances[i] = math::length(float3(position.x, position.y, position.z * height_rcp));
      }
    }
  }
  else {
    for (const int i : local_positions.index_range())
    {
      if (local_positions[i].z >= 0.0f) {
        distances[i] = 1.0f;
      }
    }
  }
}

static void scale_factors_by_local_translations(MutableSpan<float3> local_positions,
  MutableSpan<float> factors)
{
  for (const int i : local_positions.index_range()) {
    factors[i] *= local_positions[i].z;
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float3 &offset,
                       const float depth,
                       const float height,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  const SculptSession& ss = *object.sculpt;
  const StrokeCache& cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, position_data.eval, verts, factors);

  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.positions.resize(verts.size());
  const MutableSpan<float3> local_positions = tls.positions;
  calc_local_positions(mat, verts, position_data.eval, local_positions);

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_distances(depth, height, local_positions, distances);

  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(
    eBrushCurvePreset(brush.curve_preset), brush.curve, distances, 1.0f, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, position_data.eval, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;

  scale_factors_by_local_translations(local_positions, factors);
  translations_from_offset_and_factors(offset, factors, translations);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float3 &offset,
                       const float depth,
                       const float height,
                       bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, positions, factors);

  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
  }

  tls.local_positions.resize(positions.size());
  const MutableSpan<float3> local_positions = tls.local_positions;
  calc_local_positions(mat, positions, local_positions);

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_distances(depth, height, local_positions, distances);

  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(
    eBrushCurvePreset(brush.curve_preset), brush.curve, distances, 1.0f, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;

  scale_factors_by_local_translations(local_positions, factors);
  translations_from_offset_and_factors(offset, factors, translations);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4x4& mat,
                       const float3& offset,
                       const float depth,
                       const float height,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, verts, factors);
  }

  tls.local_positions.resize(positions.size());
  const MutableSpan<float3> local_positions = tls.local_positions;
  calc_local_positions(mat, positions, local_positions);

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_distances(depth, height, local_positions, distances);

  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(
    eBrushCurvePreset(brush.curve_preset), brush.curve, distances, 1.0f, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;

  scale_factors_by_local_translations(local_positions, factors);
  translations_from_offset_and_factors(offset, factors, translations);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace plane_cc

void do_plane_brush(const Depsgraph &depsgraph,
                           const Sculpt &sd,
                           Object &object,
                           const IndexMask &node_mask)
{
  const SculptSession &ss = *object.sculpt;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  if (math::is_zero(ss.cache->grab_delta_symm)) {
    return;
  }

  float3 area_no;
  float3 area_co;
  calc_brush_plane(depsgraph, brush, object, node_mask, area_no, area_co);
  SCULPT_tilt_apply_to_normal(area_no, ss.cache, brush.tilt_strength_factor);

  const float offset = SCULPT_brush_plane_offset_get(sd, ss);
  const float displace =  ss.cache->radius * offset;
  area_co += area_no * ss.cache->scale * displace;

  float4 plane;
  plane_from_point_normal_v3(plane, area_co, area_no);

  float4x4 mat = float4x4::identity();
  mat.x_axis() = math::cross(area_no, ss.cache->grab_delta_symm);
  mat.y_axis() = math::cross(area_no, float3(mat[0]));
  mat.z_axis() = area_no;
  mat.location() = area_co;
  mat = math::normalize(mat);

  const float4x4 scale = math::from_scale<float4x4>(float3(ss.cache->radius));
  float4x4 tmat = mat * scale;

  mat = math::invert(tmat);

  float3 plane_offset = -area_no;
  float depth = brush.plane_depth;
  float height = brush.plane_height;

  const bool flip = brush_flip(brush, *ss.cache) < 0.0f;

  if (flip) {
    switch (brush.plane_inversion_mode) {
      case BRUSH_PLANE_INVERT_DISPLACEMENT: {
        plane_offset = area_no;
        break;
      }
      case BRUSH_PLANE_SWAP_DEPTH_AND_HEIGHT: {
        std::swap(depth, height);
        break;
      }
    }
  }

  plane_offset *= ss.cache->radius * ss.cache->bstrength;

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *static_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh.attributes());
      const PositionDeformData position_data(depsgraph, object);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_faces(depsgraph,
          sd,
          brush,
          mat,
          plane_offset,
          brush.plane_depth,
          brush.plane_height,
          attribute_data,
          vert_normals,
          nodes[i],
          object,
          tls,
          position_data);
        bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_grids(depsgraph,
          sd,
          object,
          brush,
          mat,
          plane_offset,
          brush.plane_depth,
          brush.plane_height,
          nodes[i],
          tls);
        bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_bmesh(depsgraph,
          sd,
          object,
          brush,
          mat,
          plane_offset,
          brush.plane_depth,
          brush.plane_height,
          nodes[i],
          tls);
        bke::pbvh::update_node_bounds_bmesh(nodes[i]);
      });
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  bke::pbvh::flush_bounds_to_parents(pbvh);
}



}  // namespace blender::ed::sculpt_paint
