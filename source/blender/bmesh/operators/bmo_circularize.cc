/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 */
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
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
  /* Current postion on the plane. */
  float co_2d[2];
  /* Where it should move to on the circle. */
  float target_2d[2];
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
static bool is_valid_boundary_edge(BMEdge *e,
                                   const bool check_x,
                                   const bool check_y,
                                   const bool check_z)
{
  if (!BM_elem_flag_test(e, BM_ELEM_SELECT) || BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
    return false;
  }

  /* If edge has 2 selected faces, it's interior, not boundary. */
  if (e->l && e->l->radial_next != e->l) {
    if (BM_elem_flag_test(e->l->f, BM_ELEM_SELECT) &&
        BM_elem_flag_test(e->l->radial_next->f, BM_ELEM_SELECT))
    {
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
static bool walk_boundary_loop(BMesh * /*bm*/,
                               BMEdge *start_edge,
                               Set<BMEdge *> &visited,
                               Vector<BMVert *> &r_loop,
                               const bool check_x,
                               const bool check_y,
                               const bool check_z)
{
  /* Finds the next valid boundary edge that isn't visited. */
  auto get_next_edge = [&](BMVert *v, BMEdge *exclude_e) -> BMEdge * {
    BMIter eiter;
    BMEdge *e_next;
    BM_ITER_ELEM (e_next, &eiter, v, BM_EDGES_OF_VERT) {
      if (e_next != exclude_e && !visited.contains(e_next)) {
        if (is_valid_boundary_edge(e_next, check_x, check_y, check_z)) {
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

  r_loop.append(start_edge->v1);
  r_loop.append(start_edge->v2);
  visited.add(start_edge);

  walk(start_edge->v2, start_edge, r_loop);

  /* If the traversal forms a closed loop, the last vertex will match the first.
   * Remove the duplicate end vertex. */
  if (r_loop.size() > 2 && r_loop.first() == r_loop.last()) {
    r_loop.remove_last();
    return true;
  }

  /* If we are here, the loop is open.
   * We need to check the other direction from the start vertex. */
  Vector<BMVert *> pre_loop;
  walk(start_edge->v1, start_edge, pre_loop);

  if (!pre_loop.is_empty()) {
    std::reverse(pre_loop.begin(), pre_loop.end());

    Vector<BMVert *> full_loop;
    full_loop.reserve(pre_loop.size() + r_loop.size());
    full_loop.extend(pre_loop);
    full_loop.extend(r_loop);

    r_loop = full_loop;
  }

  return false;
}

/* Collects all valid boundary edge loops from the current selection. */
static void get_input_loops(BMesh *bm, Vector<LoopData> &r_loops, const bool check_mirror)
{
  /* If the selection has near zero extent along an axis, disable mirror plane filtering
   * for that axis so planar selections are not mistaken for symmetry boundaries. */
  float min_co[3], max_co[3];
  INIT_MINMAX(min_co, max_co);
  bool has_selection = false;

  BMIter viter;
  BMVert *v;
  BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(v, BM_ELEM_SELECT) && !BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      minmax_v3v3_v3(min_co, max_co, v->co);
      has_selection = true;
    }
  }

  if (!has_selection) {
    return;
  }

  /* These checks should only happen when there's a mirror modifier active on an
   * object. Otherwise a semi circle ends up being produced on vertices that lie
   * on axes X/Y/Z=0. */
  const bool check_x = check_mirror && (max_co[0] - min_co[0]) > MIRROR_LIMIT;
  const bool check_y = check_mirror && (max_co[1] - min_co[1]) > MIRROR_LIMIT;
  const bool check_z = check_mirror && (max_co[2] - min_co[2]) > MIRROR_LIMIT;

  Set<BMEdge *> visited;

  BMIter iter;
  BMEdge *edge;

  BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
    if (visited.contains(edge)) {
      continue;
    }
    if (!is_valid_boundary_edge(edge, check_x, check_y, check_z)) {
      continue;
    }

    LoopData ld;
    ld.is_closed = walk_boundary_loop(bm, edge, visited, ld.verts, check_x, check_y, check_z);

    if (ld.verts.size() >= 3) {
      r_loops.append(ld);
    }
  }
}

static void calculate_plane_basis(
    const Vector<BMVert *> &loop, float r_center[3], float r_normal[3], float r_p[3], float r_q[3])
{
  zero_v3(r_center);
  zero_v3(r_normal);

  if (loop.is_empty()) {
    return;
  }

  for (BMVert *v : loop) {
    add_v3_v3(r_center, v->co);
  }
  mul_v3_fl(r_center, 1.0f / loop.size());

  for (const int i : loop.index_range()) {
    BMVert *curr = loop[i];
    BMVert *next = loop[(i + 1) % loop.size()];
    add_newell_cross_v3_v3v3(r_normal, curr->co, next->co);
  }
  normalize_v3(r_normal);

  float guess[3] = {1.0f, 0.0f, 0.0f};

  if (std::abs(dot_v3v3(r_normal, guess)) > 0.99f) {
    guess[0] = 0.0f;
    guess[1] = 1.0f;
    guess[2] = 0.0f;
  }

  cross_v3_v3v3(r_p, r_normal, guess);
  normalize_v3(r_p);

  cross_v3_v3v3(r_q, r_normal, r_p);
}

static void project_loop_to_2d(const Vector<BMVert *> &loop,
                               const float center[3],
                               const float p[3],
                               const float q[3],
                               Vector<CircleVert> &r_2d_verts)
{
  r_2d_verts.reserve(loop.size());

  for (BMVert *v : loop) {
    float vec[3];
    sub_v3_v3v3(vec, v->co, center);

    CircleVert cv;
    cv.v = v;
    cv.co_2d[0] = dot_v3v3(vec, p);
    cv.co_2d[1] = dot_v3v3(vec, q);

    r_2d_verts.append(cv);
  }
}

static void calculate_circle_best_fit(const Vector<CircleVert> &verts,
                                      float r_center[2],
                                      float *r_radius,
                                      const bool is_fixed)
{
  /* If the center is locked, we skip the solver. The best fit for the fixed center
   * is simply the average radius. */
  if (is_fixed) {
    zero_v2(r_center);
    *r_radius = 0.0f;
    for (const CircleVert &cv : verts) {
      *r_radius += len_v2(cv.co_2d);
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
      const float dx = initial_x - cv.co_2d[0];
      const float dy = initial_y - cv.co_2d[1];
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

  r_center[0] = initial_x;
  r_center[1] = initial_y;
  *r_radius = initial_radius;
}

static void calculate_circle_inside_fit(const Vector<CircleVert> &verts,
                                        float r_center[2],
                                        float *r_radius,
                                        const bool is_fixed)
{
  if (is_fixed) {
    zero_v2(r_center);
  }
  else {
    float min_co[2], max_co[2];
    copy_v2_v2(min_co, verts[0].co_2d);
    copy_v2_v2(max_co, verts[0].co_2d);

    for (const CircleVert &cv : verts) {
      min_co[0] = min_ff(min_co[0], cv.co_2d[0]);
      min_co[1] = min_ff(min_co[1], cv.co_2d[1]);
      max_co[0] = max_ff(max_co[0], cv.co_2d[0]);
      max_co[1] = max_ff(max_co[1], cv.co_2d[1]);
    }

    r_center[0] = (min_co[0] + max_co[0]) * 0.5f;
    r_center[1] = (min_co[1] + max_co[1]) * 0.5f;
  }

  *r_radius = FLT_MAX;
  for (const CircleVert &cv : verts) {
    float dist = len_v2v2(r_center, cv.co_2d);
    if (dist < *r_radius) {
      *r_radius = dist;
    }
  }
}

static void calculate_target_locations(Vector<CircleVert> &verts,
                                       const float center[2],
                                       const float radius,
                                       const bool is_regular,
                                       const bool is_closed,
                                       const float rotation_angle)
{
  float total_angle = 2.0f * std::numbers::pi;
  int divisions = verts.size();
  float vec[2];

  if (!is_closed && divisions > 1) {
    total_angle = std::numbers::pi;
    divisions = verts.size() - 1;
  }

  const float step = total_angle / divisions;
  float start_angle = 0.0f;
  float sum_sin = 0.0f;
  float sum_cos = 0.0f;

  /* Using only one vertex as the basis for the start angle can skew the resulting
   * rotation of the circle in an undesirable way.
   * So instead, we calculate the circular mean of the rotation by measuring the angular
   * deviation for every vertex and averaging them to find the best fit alignment. */
  for (const int i : verts.index_range()) {
    sub_v2_v2v2(vec, verts[i].co_2d, center);
    const float angle_diff = atan2f(vec[1], vec[0]) - (step * i);
    sum_sin += sinf(angle_diff);
    sum_cos += cosf(angle_diff);
  }
  start_angle = atan2f(sum_sin, sum_cos);

  for (const int i : verts.index_range()) {
    float angle;

    if (is_regular) {
      angle = start_angle + step * i + rotation_angle;
    }
    else {
      sub_v2_v2v2(vec, verts[i].co_2d, center);
      angle = atan2f(vec[1], vec[0]) + rotation_angle;
    }

    verts[i].target_2d[0] = center[0] + cosf(angle) * radius;
    verts[i].target_2d[1] = center[1] + sinf(angle) * radius;
  }
}

static bool project_on_mesh(
    BMesh *bm, BMVert *v, const float center_pos[3], const float normal[3], float r_pos[3])
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

  BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
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
  const bool check_mirror = BMO_slot_bool_get(op->slots_in, "check_mirror");

  const bool lock_x = BMO_slot_bool_get(op->slots_in, "lock_x");
  const bool lock_y = BMO_slot_bool_get(op->slots_in, "lock_y");
  const bool lock_z = BMO_slot_bool_get(op->slots_in, "lock_z");

  Vector<LoopData> loops;
  get_input_loops(bm, loops, check_mirror);

  for (LoopData &loop_data : loops) {
    const Vector<BMVert *> &loop = loop_data.verts;

    if (loop.size() < 3) {
      continue;
    }

    float center_3d[3], normal[3], p[3], q[3];
    calculate_plane_basis(loop, center_3d, normal, p, q);

    bool is_mirrored = false;
    if (!loop_data.is_closed) {
      BMVert *v_start = loop.first();
      BMVert *v_end = loop.last();

      for (int axis = 0; axis < 3; axis++) {
        if (std::abs(v_start->co[axis]) < MIRROR_LIMIT && std::abs(v_end->co[axis]) < MIRROR_LIMIT)
        {
          is_mirrored = true;
          break;
        }
      }
    }

    /* For open loops on a symmetry plane, force the center to the midpoint of the endpoints
     * to keep the circle aligned with the mirror plane. */
    if (is_mirrored) {
      BMVert *v_start = loop.first();
      BMVert *v_end = loop.last();

      mid_v3_v3v3(center_3d, v_start->co, v_end->co);
      sub_v3_v3v3(p, v_start->co, center_3d);
      normalize_v3(p);

      cross_v3_v3v3(q, normal, p);
      normalize_v3(q);
    }

    Vector<CircleVert> circle_verts;
    project_loop_to_2d(loop, center_3d, p, q, circle_verts);

    float circle_center_2d[2];
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
      float final_pos[3];
      float offset_u[3], offset_v[3];

      mul_v3_v3fl(offset_u, p, cv.target_2d[0]);
      mul_v3_v3fl(offset_v, q, cv.target_2d[1]);

      add_v3_v3v3(final_pos, center_3d, offset_u);
      add_v3_v3(final_pos, offset_v);

      if (!flatten) {
        float projected_pos[3];
        if (project_on_mesh(bm, cv.v, final_pos, normal, projected_pos)) {
          copy_v3_v3(final_pos, projected_pos);
        }
      }

      /* If an axis is locked, restore the original coordinate. */
      if (lock_x || lock_y || lock_z) {
        const float *orig = cv.v->co;
        if (lock_x) {
          final_pos[0] = orig[0];
        }
        if (lock_y) {
          final_pos[1] = orig[1];
        }
        if (lock_z) {
          final_pos[2] = orig[2];
        }
      }

      interp_v3_v3v3(cv.v->co, cv.v->co, final_pos, factor);
    }
  }
}

}  // namespace blender
