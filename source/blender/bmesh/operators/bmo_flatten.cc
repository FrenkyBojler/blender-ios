/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Flattens vertices on a best-fitting plane.
 */

#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

static constexpr float FLATTEN_EPSILON = 1e-6f;

/**
 * Methods for determining the orientation of flattening the plane.
 */
enum FlattenMethod {
  FLATTEN_BEST_FIT = 0,
  FLATTEN_NORMAL = 1,
  FLATTEN_VIEW = 2,
};

/** A single group of vertices that will be flattened together. */
struct FlattenGroup {
  Vector<BMVert *> verts;
};

static void collect_connected_groups(BMesh *bm, Vector<FlattenGroup> &r_groups)
{
  BMIter iter;
  BMVert *v;

  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    BM_elem_flag_disable(v, BM_ELEM_INTERNAL_TAG);
  }

  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    if (!BM_elem_flag_test(v, BM_ELEM_TAG) || BM_elem_flag_test(v, BM_ELEM_HIDDEN) ||
        BM_elem_flag_test(v, BM_ELEM_INTERNAL_TAG))
    {
      continue;
    }

    FlattenGroup group;
    Vector<BMVert *> stack;
    stack.append(v);
    BM_elem_flag_enable(v, BM_ELEM_INTERNAL_TAG);

    while (!stack.is_empty()) {
      BMVert *curr = stack.pop_last();
      group.verts.append(curr);

      BMIter eiter;
      BMEdge *e;
      BM_ITER_ELEM (e, &eiter, curr, BM_EDGES_OF_VERT) {
        if (BM_elem_flag_test(e, BM_ELEM_HIDDEN) || !BM_elem_flag_test(e, BM_ELEM_TAG)) {
          continue;
        }
        BMVert *other = BM_edge_other_vert(e, curr);
        if (BM_elem_flag_test(other, BM_ELEM_INTERNAL_TAG) ||
            !BM_elem_flag_test(other, BM_ELEM_TAG))
        {
          continue;
        }
        BM_elem_flag_enable(other, BM_ELEM_INTERNAL_TAG);
        stack.append(other);
      }
    }

    r_groups.append(std::move(group));
  }
}

static float3 compute_centroid(Span<BMVert *> verts)
{
  float3 center(0.0f);
  for (BMVert *v : verts) {
    center += float3(v->co);
  }
  center /= float(verts.size());
  return center;
}

/**
 * Computes a best-fit plane normal using Newell's method.
 * Falls back to Newell's method on vertex positions for faceless meshes.
 */
static float3 compute_best_fit_normal(Span<BMVert *> verts)
{
  Set<BMFace *> visited_faces;
  float3 normal(0.0f);
  for (BMVert *v : verts) {
    BMIter fiter;
    BMFace *f;
    BM_ITER_ELEM (f, &fiter, v, BM_FACES_OF_VERT) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN) || !visited_faces.add(f)) {
        continue;
      }
      BMIter liter;
      BMLoop *l;
      BM_ITER_ELEM (l, &liter, f, BM_LOOPS_OF_FACE) {
        add_newell_cross_v3_v3v3(normal, l->prev->v->co, l->v->co);
      }
    }
  }

  if (math::length(normal) > FLATTEN_EPSILON) {
    return math::normalize(normal);
  }

  normal = float3(0.0f);
  for (const int i : verts.index_range().drop_back(1)) {
    add_newell_cross_v3_v3v3(normal, verts[i]->co, verts[i + 1]->co);
  }
  /* Newell's method requires a closed loop. */
  if (verts.size() >= 2) {
    add_newell_cross_v3_v3v3(normal, verts[verts.size() - 1]->co, verts[0]->co);
  }
  return math::normalize(normal);
}

static float3 compute_average_vertex_normal(Span<BMVert *> verts)
{
  float3 normal(0.0f);
  for (BMVert *v : verts) {
    normal += float3(v->no);
  }
  float length = math::length(normal);
  if (length > FLATTEN_EPSILON) {
    return normal / length;
  }
  return float3(0.0f, 0.0f, 1.0f);
}

void bmo_flatten_exec(BMesh *bm, BMOperator *op)
{
  const float factor = BMO_slot_float_get(op->slots_in, "factor");
  const int method = BMO_slot_int_get(op->slots_in, "method");
  const bool lock_x = BMO_slot_bool_get(op->slots_in, "lock_x");
  const bool lock_y = BMO_slot_bool_get(op->slots_in, "lock_y");
  const bool lock_z = BMO_slot_bool_get(op->slots_in, "lock_z");

  float3 view_direction(0.0f, 0.0f, 1.0f);
  if (method == FLATTEN_VIEW) {
    BMO_slot_vec_get(op->slots_in, "view_normal", view_direction);
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(
      bm, op->slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  Vector<FlattenGroup> groups;
  collect_connected_groups(bm, groups);

  for (const FlattenGroup &group : groups) {
    float3 center = compute_centroid(group.verts);
    float3 normal;

    switch (method) {
      case FLATTEN_BEST_FIT:
        normal = compute_best_fit_normal(group.verts);
        break;
      case FLATTEN_NORMAL:
        normal = compute_average_vertex_normal(group.verts);
        break;
      case FLATTEN_VIEW:
        normal = view_direction;
        break;
      default:
        BLI_assert_unreachable();
        normal = compute_best_fit_normal(group.verts);
        break;
    }

    for (BMVert *v : group.verts) {
      float3 co(v->co);
      float3 projected = co - math::dot(co - center, normal) * normal;

      if (lock_x) {
        projected.x = co.x;
      }
      if (lock_y) {
        projected.y = co.y;
      }
      if (lock_z) {
        projected.z = co.z;
      }

      float3 final_pos = math::interpolate(co, projected, factor);
      copy_v3_v3(v->co, final_pos);
    }
  }
}

}  // namespace blender
