/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/brushes/types.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_bvhutils.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh_brush_common.hh"
#include "editors/sculpt_paint/sculpt_automask.hh"
#include "editors/sculpt_paint/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint {

inline namespace scene_project_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float> hit_distances;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

static void object_raycast(const Object &object,
                           const float3 &normal,
                           const Span<float3> positions,
                           const Span<float> factors,
                           const MutableSpan<float> r_hit_distances)
{
  const Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::BVHTreeFromMesh tree_data = mesh.bvh_corner_tris();

  if (tree_data.tree == nullptr) {
    return;
  }

  for (const int i : positions.index_range()) {
    if (factors[i] == 0.0f) {
      continue;
    }

    BVHTreeRayHit hit;
    hit.dist = std::numeric_limits<float>::max();

    BLI_bvhtree_ray_cast(
        tree_data.tree, positions[i], normal, 0.0f, &hit, tree_data.raycast_callback, &tree_data);

    r_hit_distances[i] = math::min(r_hit_distances[i], hit.dist);
  }
}

static void scene_raycast(const Span<Object *> target_objects,
                          const float3 &normal,
                          const Span<float3> positions,
                          const Span<float> factors,
                          const MutableSpan<float> r_hit_distances)
{
  r_hit_distances.fill(std::numeric_limits<float>::max());

  for (const int i : target_objects.index_range()) {
    object_raycast(*target_objects[i], normal, positions, factors, r_hit_distances);
  }

  for (const int i : r_hit_distances.index_range()) {
    if (r_hit_distances[i] == std::numeric_limits<float>::max()) {
      r_hit_distances[i] = 0.0f;
    }
  }
}

static void calc_world_positions(const float4x4 mat,
                                 const Span<int> verts,
                                 const Span<float3> object_positions,
                                 const MutableSpan<float3> r_world_positions)
{
  for (const int i : verts.index_range()) {
    r_world_positions[i] = math::transform_point(mat, object_positions[verts[i]]);
  }
}

static void calc_world_positions(const float4x4 mat,
                                 const Span<float3> object_positions,
                                 const MutableSpan<float3> r_world_positions)
{
  for (const int i : object_positions.index_range()) {
    r_world_positions[i] = math::transform_point(mat, object_positions[i]);
  }
}

static void calc_world_translations(const float3 &normal,
                                    const Span<float> factors,
                                    const Span<float> hit_distances,
                                    const MutableSpan<float3> r_translations)
{
  for (const int i : factors.index_range()) {
    r_translations[i] = normal * hit_distances[i] * factors[i];
  }
}

static void calc_object_translations(const float4x4 &mat,
                                     const Span<float3> world_translations,
                                     const MutableSpan<float3> r_object_translations)
{
  for (const int i : world_translations.index_range()) {
    r_object_translations[i] = math::transform_direction(mat, world_translations[i]);
  }
}

static float3 calc_world_normal(const float4x4 &mat, const Brush &brush, const StrokeCache &cache)
{
  float3 object_normal;

  switch (brush.project_direction_type) {
    case BRUSH_PROJECT_DIRECTION_VIEW_NORMAL:
      object_normal = -cache.view_normal_symm;
      break;
    case BRUSH_PROJECT_DIRECTION_PLANE_NORMAL:
      object_normal = -cache.sculpt_normal_symm;
      break;
  }

  if (cache.initial_direction_flipped) {
    object_normal *= -1.0f;
  }

  return math::transform_direction(mat, object_normal);
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float strength,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  SculptSession &ss = *object.sculpt;
  const Span<int> verts = node.verts();

  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   position_data.eval,
                                   vert_normals,
                                   node,
                                   tls.factors,
                                   tls.distances);

  tls.positions.resize(verts.size());
  const MutableSpan<float3> world_positions = tls.positions;
  calc_world_positions(object.object_to_world(), verts, position_data.eval, world_positions);

  const float3 world_normal = calc_world_normal(object.object_to_world(), brush, *ss.cache);

  tls.hit_distances.resize(verts.size());
  const MutableSpan<float> hit_distances = tls.hit_distances;
  scene_raycast(
      ss.cache->target_objects, world_normal, world_positions, tls.factors, hit_distances);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> world_translations = tls.translations;
  calc_world_translations(world_normal, tls.factors, hit_distances, world_translations);

  const MutableSpan<float3> object_translations = world_translations;
  calc_object_translations(object.world_to_object(), world_translations, object_translations);

  scale_translations(object_translations, strength);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, object_translations);
  position_data.deform(object_translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan<float3> positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  const MutableSpan<float3> world_positions = positions;
  calc_world_positions(object.object_to_world(), positions, world_positions);

  const float3 world_normal = calc_world_normal(object.object_to_world(), brush, *ss.cache);

  tls.hit_distances.resize(positions.size());
  const MutableSpan<float> hit_distances = tls.hit_distances;
  scene_raycast(
      ss.cache->target_objects, world_normal, world_positions, tls.factors, hit_distances);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> world_translations = tls.translations;
  calc_world_translations(world_normal, tls.factors, hit_distances, world_translations);

  const MutableSpan<float3> object_translations = world_translations;
  calc_object_translations(object.world_to_object(), world_translations, object_translations);

  scale_translations(object_translations, strength);

  clip_and_lock_translations(sd, ss, positions, object_translations);
  apply_translations(object_translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.sculpt;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  const MutableSpan<float3> world_positions = positions;
  calc_world_positions(object.object_to_world(), positions, world_positions);

  const float3 world_normal = calc_world_normal(object.object_to_world(), brush, *ss.cache);

  tls.hit_distances.resize(positions.size());
  const MutableSpan<float> hit_distances = tls.hit_distances;
  scene_raycast(
      ss.cache->target_objects, world_normal, world_positions, tls.factors, hit_distances);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> world_translations = tls.translations;
  calc_world_translations(world_normal, tls.factors, hit_distances, world_translations);

  const MutableSpan<float3> object_translations = world_translations;
  calc_object_translations(object.world_to_object(), world_translations, object_translations);

  scale_translations(object_translations, strength);

  clip_and_lock_translations(sd, ss, positions, object_translations);
  apply_translations(object_translations, verts);
}

}  // namespace scene_project_cc

void do_scene_project_brush(const Depsgraph &depsgraph,
                            const Sculpt &sd,
                            Object &object,
                            const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const StrokeCache &cache = *object.sculpt->cache;

  const float strength = cache.radius * cache.bstrength;

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *static_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_faces(depsgraph,
                   sd,
                   brush,
                   strength,
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
        calc_grids(depsgraph, sd, object, brush, strength, nodes[i], tls);
        bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_bmesh(depsgraph, sd, object, brush, strength, nodes[i], tls);
        bke::pbvh::update_node_bounds_bmesh(nodes[i]);
      });
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}
}  // namespace blender::ed::sculpt_paint
