/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 */
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include <numbers>

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

constexpr int NLLS_MAX_ITERATIONS = 500;
constexpr float MIRROR_LIMIT = 0.001f;
constexpr float CIRCULARIZE_EPSILON = 1e-6f;

/* Holds data for a vertex projected onto the local plane. */
struct CircleVert {
  BMVert *v;
  /* Current position on the plane. */
  float2 co_2d;
  /* Where it should move to on the circle. */
  float2 target_2d;
};

/* Stores the geometry loop and whether it forms a closed circle or open chain. */
struct LoopData {
  Vector<BMVert *> verts;
  bool is_closed;
};

/* Detects whether an edge should be considered a valid boundary
 * edge for circularization.
 * Valid boundary edges are edges that are selected, not hidden
 * and are not interior. They lie on the boundary between a selected
 * face and an unselected face and do not lie on the mirror plane. */
static bool is_valid_boundary_edge(
    BMEdge *e, const char hflag, const bool check_x, const bool check_y, const bool check_z)
{
  if (!BM_elem_flag_test(e, hflag) || BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
    return false;
  }

  /* If edge has 2 selected faces, it's interior, not boundary. */
  if (e->l && e->l->radial_next != e->l) {
    if (BM_elem_flag_test(e->l->f, hflag) && BM_elem_flag_test(e->l->radial_next->f, hflag)) {
      return false;
    }
  }

  /* If both vertices of an edge lie close to the same coordinate plane
   * (X = 0, Y = 0, or Z = 0), the edge lies on a mirror plane and is not
   * considered a valid boundary edge. */

  /* YZ Plane */
  if (check_x && std::abs(e->v1->co[0]) < MIRROR_LIMIT && std::abs(e->v2->co[0]) < MIRROR_LIMIT) {
    return false;
  }
  /* XZ Plane */
  if (check_y && std::abs(e->v1->co[1]) < MIRROR_LIMIT && std::abs(e->v2->co[1]) < MIRROR_LIMIT) {
    return false;
  }
  /* XY Plane */
  if (check_z && std::abs(e->v1->co[2]) < MIRROR_LIMIT && std::abs(e->v2->co[2]) < MIRROR_LIMIT) {
    return false;
  }

  return true;
}

/* Traverses a connected path of boundary edges to form a continuous sequence of vertices.
 * This function handles two cases:
 * 1. Closed loops: walks until the traversal returns to the start vertex.
 * 2. Open chains: walks in one direction until a dead end, then walks in the
 * opposite direction from the start edge and merges the results.
 */
static LoopData walk_boundary_loop(BMEdge *start_edge,
                                   Set<BMEdge *> &visited,
                                   const char hflag,
                                   const bool check_x,
                                   const bool check_y,
                                   const bool check_z)
{
  LoopData loop_data;
  /* Finds the next valid boundary edge that isn't visited. */
  auto get_next_edge = [&](BMVert *v, BMEdge *exclude_e) -> BMEdge * {
    BMIter eiter;
    BMEdge *e_next;
    BM_ITER_ELEM (e_next, &eiter, v, BM_EDGES_OF_VERT) {
      if (e_next != exclude_e && !visited.contains(e_next)) {
        if (is_valid_boundary_edge(e_next, hflag, check_x, check_y, check_z)) {
          return e_next;
        }
      }
    }
    return nullptr;
  };

  /* Walks in one direction until a dead end. */
  auto walk = [&](BMVert *curr_v, BMEdge *curr_e, Vector<BMVert *> &list) {
    while (true) {
      BMEdge *next_e = get_next_edge(curr_v, curr_e);
      if (!next_e) {
        break;
      }

      /* Move to next vertex. */
      curr_v = BM_edge_other_vert(next_e, curr_v);
      curr_e = next_e;

      list.append(curr_v);
      visited.add(curr_e);
    }
  };

  loop_data.verts.append(start_edge->v1);
  loop_data.verts.append(start_edge->v2);
  visited.add(start_edge);

  walk(start_edge->v2, start_edge, loop_data.verts);

  /* If the traversal forms a closed loop, the last vertex will match the first.
   * Remove the duplicate end vertex. */
  if (loop_data.verts.size() > 2 && loop_data.verts.first() == loop_data.verts.last()) {
    loop_data.verts.remove_last();
    loop_data.is_closed = true;
    return loop_data;
  }

  /* If we are here, the loop is open.
   * We need to check the other direction from the start vertex. */
  Vector<BMVert *> pre_loop;
  walk(start_edge->v1, start_edge, pre_loop);

  if (!pre_loop.is_empty()) {
    std::reverse(pre_loop.begin(), pre_loop.end());

    pre_loop.extend(loop_data.verts);
    loop_data.verts = std::move(pre_loop);
  }

  loop_data.is_closed = false;
  return loop_data;
}

/* Collects all valid boundary edge loops from the current selection. */
static void get_input_loops(BMesh *bm,
                            Vector<LoopData> &r_loops,
                            const char hflag,
                            const bool check_x,
                            const bool check_y,
                            const bool check_z)
{
  Set<BMEdge *> visited;
  BMIter iter;
  BMEdge *edge;

  BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
    if (visited.contains(edge)) {
      continue;
    }
    if (!is_valid_boundary_edge(edge, hflag, check_x, check_y, check_z)) {
      continue;
    }

    LoopData ld = walk_boundary_loop(edge, visited, hflag, check_x, check_y, check_z);
    if (ld.verts.size() >= 3) {
      r_loops.append(ld);
    }
  }
}

/* Computes the local coordinate system defining the 2D plane of the vertex loop. */
static void calculate_plane_basis(
    Span<BMVert *> loop, float3 &r_center, float3 &r_normal, float3 &r_p, float3 &r_q)
{
  r_center = float3(0.0f);
  r_normal = float3(0.0f);

  for (BMVert *v : loop) {
    r_center += float3(v->co);
  }
  r_center /= float(loop.size());

  /* Compute a best fit plane normal for the loop using Newell's method. */
  for (const int i : loop.index_range()) {
    BMVert *curr = loop[i];
    BMVert *next = loop[(i + 1) % loop.size()];
    add_newell_cross_v3_v3v3(r_normal, curr->co, next->co);
  }
  r_normal = math::normalize(r_normal);
  float3 guess = float3(1.0f, 0.0f, 0.0f);

  /* If r_normal is parallel to (1,0,0) cross product would be zero.
   * In that case, we switch the guess to the y axis to allow a valid
   * perpendicular vector to be found. */
  if (std::abs(math::dot(r_normal, guess)) > 0.99f) {
    guess = float3(0.0f, 1.0f, 0.0f);
  }

  r_p = math::cross(r_normal, guess);
  r_p = math::normalize(r_p);
  r_q = math::cross(r_normal, r_p);
}

/* Projects 3D vertex coordinates onto a local 2D plane defined by the P and Q basis vectors. */
static void project_loop_to_2d(Span<BMVert *> loop,
                               const float3 &center,
                               const float3 &p,
                               const float3 &q,
                               Vector<CircleVert> &r_2d_verts)
{
  r_2d_verts.reserve(loop.size());
  for (BMVert *v : loop) {
    float3 vec = float3(v->co) - center;
    CircleVert cv{.v = v, .co_2d = {math::dot(vec, p), math::dot(vec, q)}};
    r_2d_verts.append(cv);
  }
}

static void calculate_circle_best_fit(Span<CircleVert> verts,
                                      float2 &r_center,
                                      float *r_radius,
                                      const bool is_fixed)
{
  /* If the center is locked, we skip the solver. The best fit for the fixed center
   * is simply the average radius. */
  if (is_fixed) {
    r_center = float2(0.0f);
    *r_radius = 0.0f;
    for (const CircleVert &cv : verts) {
      *r_radius += math::length(cv.co_2d);
    }
    *r_radius /= verts.size();
    return;
  }

  /* Initial guesses. */
  float initial_x = 0.0f;
  float initial_y = 0.0f;
  float initial_radius = 1.0f;

  for (int iter = 0; iter < NLLS_MAX_ITERATIONS; iter++) {
    float normal_matrix[3][3];
    float jacobian_transpose_residual[3];

    zero_m3(normal_matrix);
    zero_v3(jacobian_transpose_residual);

    for (const CircleVert &cv : verts) {
      const float dx = initial_x - cv.co_2d.x;
      const float dy = initial_y - cv.co_2d.y;
      const float distance = sqrtf(dx * dx + dy * dy);

      const float j_row[3] = {dx / distance, dy / distance, -1.0f};
      const float residual = initial_radius - distance;

      for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
          normal_matrix[row][col] += j_row[row] * j_row[col];
        }
        jacobian_transpose_residual[row] += j_row[row] * residual;
      }
    }

    float inverse_normal_matrix[3][3];
    if (!invert_m3_m3(inverse_normal_matrix, normal_matrix)) {
      break;
    }

    float delta[3];
    mul_v3_m3v3(delta, inverse_normal_matrix, jacobian_transpose_residual);

    initial_x += delta[0];
    initial_y += delta[1];
    initial_radius += delta[2];

    /* Check for convergence to stop iterating if we're close enough to the optimal
     * solution. */
    if (std::abs(delta[0]) < CIRCULARIZE_EPSILON && std::abs(delta[1]) < CIRCULARIZE_EPSILON &&
        std::abs(delta[2]) < CIRCULARIZE_EPSILON)
    {
      break;
    }
  }

  r_center.x = initial_x;
  r_center.y = initial_y;
  *r_radius = initial_radius;
}

static void calculate_circle_inside_fit(Span<CircleVert> verts,
                                        float2 &r_center,
                                        float *r_radius,
                                        const bool is_fixed)
{
  if (is_fixed) {
    r_center = float2(0.0f);
  }
  else {
    float2 min_co = verts[0].co_2d;
    float2 max_co = verts[0].co_2d;

    for (const CircleVert &cv : verts) {
      min_co = math::min(min_co, cv.co_2d);
      max_co = math::max(max_co, cv.co_2d);
    }

    r_center = (min_co + max_co) * 0.5f;
  }

  *r_radius = FLT_MAX;
  for (const CircleVert &cv : verts) {
    const float dist = math::distance(r_center, cv.co_2d);
    if (dist < *r_radius) {
      *r_radius = dist;
    }
  }
}

static void calculate_target_locations(MutableSpan<CircleVert> verts,
                                       const float2 &center,
                                       const float radius,
                                       const bool is_regular,
                                       const bool is_closed,
                                       const float rotation_angle)
{
  float step = 0.0f;
  float start_angle = 0.0f;

  if (is_regular) {
    float total_angle = 2.0f * std::numbers::pi;
    int divisions = verts.size();

    /* For open loops, we calculate the total angle obtained by traversing
     * the chain of vertices. Unlike closed loops whose total angle is 2*Pi,
     * we cannot assume Pi for an open loop because it might span any amount
     * of the circle. */
    if (!is_closed && divisions > 1) {
      total_angle = 0.0f;
      divisions = verts.size() - 1;

      float2 vec_prev = verts[0].co_2d - center;
      vec_prev = math::normalize(vec_prev);

      for (const int i : verts.index_range()) {
        float2 vec_curr = verts[i].co_2d - center;
        vec_curr = math::normalize(vec_curr);

        total_angle += angle_normalized_v2v2(vec_prev, vec_curr);
        vec_prev = vec_curr;
      }
    }

    step = total_angle / divisions;
    float sum_sin = 0.0f;
    float sum_cos = 0.0f;

    /* Using only one vertex as the basis for the start angle can skew
     * the resulting rotation of the circle in an undesirable way.
     * So instead, we calculate the circular mean of the rotation by measuring
     * the angular deviation for every vertex and averaging them to find the best
     * fit alignment. */
    for (const int i : verts.index_range()) {
      float2 vec = verts[i].co_2d - center;
      const float angle_diff = atan2f(vec.y, vec.x) - (step * i);
      sum_sin += sinf(angle_diff);
      sum_cos += cosf(angle_diff);
    }
    start_angle = atan2f(sum_sin, sum_cos);
  }

  for (const int i : verts.index_range()) {
    float angle;

    if (is_regular) {
      angle = start_angle + step * i + rotation_angle;
    }
    else {
      float2 vec = verts[i].co_2d - center;
      angle = atan2f(vec.y, vec.x) + rotation_angle;
    }

    verts[i].target_2d.x = center.x + cosf(angle) * radius;
    verts[i].target_2d.y = center.y + sinf(angle) * radius;
  }
}

struct NearestTriUserData {
  Span<std::array<BMLoop *, 3>> looptris;
};

/* Callback for BLI_bvhtree_find_nearest. Finds the closest point on the given triangle. */
static void nearest_tri_cb(void *userdata, int index, const float co[3], BVHTreeNearest *nearest)
{
  const NearestTriUserData *data = static_cast<const NearestTriUserData *>(userdata);
  const std::array<BMLoop *, 3> &ltri = data->looptris[index];

  float3 closest;
  closest_on_tri_to_point_v3(closest, co, ltri[0]->v->co, ltri[1]->v->co, ltri[2]->v->co);
  const float dist_sq = math::distance_squared(float3(co), closest);
  if (dist_sq < nearest->dist_sq) {
    nearest->dist_sq = dist_sq;
    nearest->index = index;
    copy_v3_v3(nearest->co, closest);
  }
}

static bool project_on_mesh(BVHTree *bvh_tree,
                            NearestTriUserData *bvh_data,
                            BMVert *v,
                            const float center_pos[3],
                            const float normal[3],
                            float r_pos[3])
{
  if (equals_v3v3(v->co, center_pos)) {
    copy_v3_v3(r_pos, center_pos);
    return true;
  }

  float vec[3];
  sub_v3_v3v3(vec, center_pos, v->co);
  const float angle = angle_v3v3(vec, normal);
  if (std::abs(angle) < CIRCULARIZE_EPSILON ||
      std::abs(std::numbers::pi - angle) < CIRCULARIZE_EPSILON)
  {
    copy_v3_v3(r_pos, v->co);
    return true;
  }

  const float *rays[2] = {normal, nullptr};
  float neg_normal[3];
  negate_v3_v3(neg_normal, normal);
  rays[1] = neg_normal;

  float best_dist = FLT_MAX;
  bool found = false;

  auto test_tri = [&](BMVert *v1, BMVert *v2, BMVert *v3) {
    for (int i = 0; i < 2; i++) {
      float lambda;
      float uv[2];
      if (isect_ray_tri_v3(center_pos, rays[i], v1->co, v2->co, v3->co, &lambda, uv)) {
        float hit_pos[3];
        madd_v3_v3v3fl(hit_pos, center_pos, rays[i], lambda);
        const float dist = len_squared_v3v3(center_pos, hit_pos);
        if (dist < best_dist) {
          best_dist = dist;
          copy_v3_v3(r_pos, hit_pos);
          found = true;
        }
      }
    }
  };

  BMIter fiter;
  BMFace *f;
  BM_ITER_ELEM (f, &fiter, v, BM_FACES_OF_VERT) {
    if (f->len < 3 || BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }
    BMLoop *l_start = f->l_first;
    BMVert *v1 = l_start->v;
    BMVert *v2 = l_start->next->v;
    BMVert *v3 = l_start->next->next->v;
    test_tri(v1, v2, v3);
    if (f->len == 4) {
      BMVert *v4 = l_start->prev->v;
      test_tri(v1, v3, v4);
    }
  }

  if (found) {
    return true;
  }

  BMIter eiter;
  BMEdge *e;
  BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
    float closest[3];
    closest_to_line_v3(closest, center_pos, e->v1->co, e->v2->co);
    const float fac = line_point_factor_v3(closest, e->v1->co, e->v2->co);
    if (fac > CIRCULARIZE_EPSILON && fac < 1.0f - CIRCULARIZE_EPSILON) {
      best_dist = len_squared_v3v3(center_pos, closest);
      copy_v3_v3(r_pos, closest);
      found = true;
      break;
    }
  }

  if (found) {
    return true;
  }

  if (bvh_tree) {
    BVHTreeNearest nearest;
    nearest.dist_sq = FLT_MAX;
    nearest.index = -1;
    BLI_bvhtree_find_nearest(bvh_tree, center_pos, &nearest, nearest_tri_cb, bvh_data);
    if (nearest.index != -1) {
      copy_v3_v3(r_pos, nearest.co);
      return true;
    }
  }

  copy_v3_v3(r_pos, center_pos);
  return true;
}

void bmo_circularize_exec(BMesh *bm, BMOperator *op)
{
  const float factor = BMO_slot_float_get(op->slots_in, "factor");
  const float custom_radius = BMO_slot_float_get(op->slots_in, "custom_radius");
  const float angle = BMO_slot_float_get(op->slots_in, "angle");
  const int fit_method = BMO_slot_int_get(op->slots_in, "fit_method");
  const bool flatten = BMO_slot_bool_get(op->slots_in, "flatten");
  const bool regular = BMO_slot_bool_get(op->slots_in, "regular");
  const bool mirror_x = BMO_slot_bool_get(op->slots_in, "mirror_x");
  const bool mirror_y = BMO_slot_bool_get(op->slots_in, "mirror_y");
  const bool mirror_z = BMO_slot_bool_get(op->slots_in, "mirror_z");

  const bool lock_x = BMO_slot_bool_get(op->slots_in, "lock_x");
  const bool lock_y = BMO_slot_bool_get(op->slots_in, "lock_y");
  const bool lock_z = BMO_slot_bool_get(op->slots_in, "lock_z");

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(
      bm, op->slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  Vector<LoopData> loops;
  get_input_loops(bm, loops, BM_ELEM_TAG, mirror_x, mirror_y, mirror_z);

  /* Builds a BVH tree when flatten is disabled. Without this we would have to iterate
   * over every face in the mesh for every vertex which is too slow. */
  Vector<std::array<BMLoop *, 3>> looptris;
  BVHTree *bvh_tree = nullptr;
  NearestTriUserData bvh_data = {};

  if (!flatten) {
    const int tot_tri = poly_to_tri_count(bm->totface, bm->totloop);
    looptris.reinitialize(tot_tri);
    BM_mesh_calc_tessellation(bm, looptris);

    bvh_tree = BLI_bvhtree_new(tot_tri, 0.0f, 8, 8);
    for (const int i : looptris.index_range()) {
      const std::array<BMLoop *, 3> &ltri = looptris[i];
      float3 cos[3] = {float3(ltri[0]->v->co), float3(ltri[1]->v->co), float3(ltri[2]->v->co)};
      BLI_bvhtree_insert(bvh_tree, i, reinterpret_cast<float *>(cos), 3);
    }
    BLI_bvhtree_balance(bvh_tree);
    bvh_data.looptris = looptris;
  }

  for (LoopData &loop_data : loops) {
    const Vector<BMVert *> &loop = loop_data.verts;

    if (loop.size() < 3) {
      continue;
    }

    float3 center_3d, normal, p, q;
    calculate_plane_basis(loop, center_3d, normal, p, q);

    bool is_mirrored = false;
    int mirror_axis = -1;

    if (!loop_data.is_closed) {
      BMVert *v_start = loop.first();
      BMVert *v_end = loop.last();

      if (mirror_x && std::abs(v_start->co[0]) < MIRROR_LIMIT &&
          std::abs(v_end->co[0]) < MIRROR_LIMIT)
      {
        is_mirrored = true;
        mirror_axis = 0;
      }
      else if (mirror_y && std::abs(v_start->co[1]) < MIRROR_LIMIT &&
               std::abs(v_end->co[1]) < MIRROR_LIMIT)
      {
        is_mirrored = true;
        mirror_axis = 1;
      }
      else if (mirror_z && std::abs(v_start->co[2]) < MIRROR_LIMIT &&
               std::abs(v_end->co[2]) < MIRROR_LIMIT)
      {
        is_mirrored = true;
        mirror_axis = 2;
      }
    }

    /* For open loops on a symmetry plane, force the center to the midpoint of the endpoints
     * to keep the circle aligned with the mirror plane. */
    if (is_mirrored) {
      BMVert *v_start = loop.first();
      BMVert *v_end = loop.last();

      center_3d = math::midpoint(float3(v_start->co), float3(v_end->co));
      p = math::normalize(float3(v_start->co) - center_3d);
      q = math::normalize(math::cross(normal, p));
    }

    Vector<CircleVert> circle_verts;
    project_loop_to_2d(loop, center_3d, p, q, circle_verts);

    float2 circle_center_2d;
    float radius;

    if (fit_method == 1) {
      calculate_circle_inside_fit(circle_verts, circle_center_2d, &radius, is_mirrored);
    }
    else {
      calculate_circle_best_fit(circle_verts, circle_center_2d, &radius, is_mirrored);
    }

    if (custom_radius > 0.0f) {
      radius = custom_radius;
    }

    calculate_target_locations(
        circle_verts, circle_center_2d, radius, regular, loop_data.is_closed, angle);

    for (const CircleVert &cv : circle_verts) {
      float3 final_pos = center_3d + p * cv.target_2d.x + q * cv.target_2d.y;

      if (!flatten) {
        float projected_pos[3];
        if (project_on_mesh(bvh_tree, &bvh_data, cv.v, final_pos, normal, projected_pos)) {
          final_pos = float3(projected_pos);
        }
      }

      /* If this vertex is an endpoint of a mirrored loop, force it
       * exactly to 0.0 on the mirror axis.
       * There are some cases where a slight floating point drift ends up being
       * produced which prevents the mirror modifier from merging vertices. */
      if (is_mirrored && mirror_axis != -1) {
        if (cv.v == loop.first() || cv.v == loop.last()) {
          final_pos[mirror_axis] = 0.0f;
        }
      }

      /* If an axis is locked, restore the original coordinate. */
      if (lock_x || lock_y || lock_z) {
        const float *orig = cv.v->co;
        if (lock_x) {
          final_pos.x = orig[0];
        }
        if (lock_y) {
          final_pos.y = orig[1];
        }
        if (lock_z) {
          final_pos.z = orig[2];
        }
      }

      interp_v3_v3v3(cv.v->co, cv.v->co, final_pos, factor);
    }
  }

  /* There would be a memory leak if this isn't freed.  */
  if (bvh_tree) {
    BLI_bvhtree_free(bvh_tree);
  }
}

}  // namespace blender
