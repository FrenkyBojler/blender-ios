/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 */

#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

using namespace blender;

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
  /* If this loop was generated from a single selected vertex, this pointer is set. */
  BMVert *center_vert = nullptr;
};

static bool is_valid_boundary_edge(BMEdge *e)
{
  if (!BM_elem_flag_test(e, BM_ELEM_SELECT)) {
    return false;
  }
  if (BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
    return false;
  }

  if (e->l && e->l->radial_next != e->l) {
    bool f1_sel = BM_elem_flag_test(e->l->f, BM_ELEM_SELECT);
    bool f2_sel = BM_elem_flag_test(e->l->radial_next->f, BM_ELEM_SELECT);

    if (f1_sel && f2_sel) {
      return false;
    }
  }
  return true;
}

static bool walk_boundary_loop(BMesh * /*bm*/,
                               BMEdge *start_edge,
                               Set<BMEdge *> &visited,
                               Vector<BMVert *> &r_loop)
{
  BMVert *v_curr = start_edge->v1;
  BMEdge *e_curr = start_edge;

  r_loop.append(v_curr);
  visited.add(e_curr);

  bool is_closed = false;
  bool found_next = true;

  while (found_next) {
    found_next = false;

    BMVert *v_next = BM_edge_other_vert(e_curr, v_curr);

    if (!r_loop.is_empty() && v_next == r_loop[0]) {
      is_closed = true;
      break;
    }

    r_loop.append(v_next);
    v_curr = v_next;

    BMIter eiter;
    BMEdge *e_next;
    BM_ITER_ELEM (e_next, &eiter, v_curr, BM_EDGES_OF_VERT) {
      if (e_next == e_curr) {
        continue;
      }
      if (visited.contains(e_next)) {
        continue;
      }

      if (is_valid_boundary_edge(e_next)) {
        e_curr = e_next;
        visited.add(e_curr);
        found_next = true;
        break;
      }
    }
  }
  return is_closed;
}

static void sort_fan_edges(const Set<BMEdge *> &edges, Vector<BMVert *> &r_loop)
{
  if (edges.is_empty()) {
    return;
  }

  /* Start with an arbitrary edge. */
  BMEdge *start_edge = *edges.begin();
  BMEdge *curr_edge = start_edge;
  BMVert *curr_vert = start_edge->v1;

  Set<BMEdge *> processed;

  bool found_next = true;

  while (found_next) {
    found_next = false;
    processed.add(curr_edge);
    r_loop.append(curr_vert);

    /* Traverse to the other side of the edge. */
    curr_vert = BM_edge_other_vert(curr_edge, curr_vert);

    /* Check if we closed the loop. */
    if (curr_vert == start_edge->v1) {
      break;
    }

    /* Find the next edge in our specific set. */
    BMIter eiter;
    BMEdge *next_edge;
    BM_ITER_ELEM (next_edge, &eiter, curr_vert, BM_EDGES_OF_VERT) {
      if (next_edge != curr_edge && edges.contains(next_edge) && !processed.contains(next_edge)) {
        curr_edge = next_edge;
        found_next = true;
        break;
      }
    }
  }
}

static void get_single_vertex_loops(BMesh *bm, Vector<LoopData> &r_loops)
{
  BMIter viter;
  BMVert *v;
  BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
    if (!BM_elem_flag_test(v, BM_ELEM_SELECT) || BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      continue;
    }

    /* Check if this is an isolated selection. */
    bool has_selected_edge = false;
    BMIter eiter;
    BMEdge *e;
    BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
      if (BM_elem_flag_test(e, BM_ELEM_SELECT)) {
        has_selected_edge = true;
        break;
      }
    }

    if (has_selected_edge) {
      continue;
    }

    /* Collect boundary edges of the face fan surrounding this vertex. */
    Set<BMEdge *> fan_edges;
    BMIter fiter;
    BMFace *f;
    BM_ITER_ELEM (f, &fiter, v, BM_FACES_OF_VERT) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      /* Add edges of this face that do not touch the center vertex. */
      BMLoop *l_start = f->l_first;
      BMLoop *l_iter = l_start;
      do {
        if (l_iter->v != v && l_iter->next->v != v) {
          fan_edges.add(l_iter->e);
        }
      } while ((l_iter = l_iter->next) != l_start);
    }

    if (fan_edges.size() >= 3) {
      LoopData ld;
      sort_fan_edges(fan_edges, ld.verts);

      /* Only valid if we formed a proper loop around the vertex. */
      if (ld.verts.size() >= 3) {
        /* Single vertex surroundings are conceptually closed loops. */
        ld.is_closed = true;
        ld.center_vert = v;
        r_loops.append(ld);
      }
    }
  }
}

static void get_input_loops(BMesh *bm, Vector<LoopData> &r_loops)
{
  Set<BMEdge *> visited;

  BMIter iter;
  BMEdge *edge;

  BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
    if (visited.contains(edge)) {
      continue;
    }
    if (!is_valid_boundary_edge(edge)) {
      continue;
    }

    LoopData ld;
    ld.is_closed = walk_boundary_loop(bm, edge, visited, ld.verts);

    if (ld.verts.size() >= 3) {
      r_loops.append(ld);
    }
  }

  get_single_vertex_loops(bm, r_loops);
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

  for (int i = 0; i < loop.size(); i++) {
    BMVert *curr = loop[i];
    BMVert *next = loop[(i + 1) % loop.size()];

    r_normal[0] += (curr->co[1] - next->co[1]) * (curr->co[2] + next->co[2]);
    r_normal[1] += (curr->co[2] - next->co[2]) * (curr->co[0] + next->co[0]);
    r_normal[2] += (curr->co[0] - next->co[0]) * (curr->co[1] + next->co[1]);
  }
  normalize_v3(r_normal);

  float guess[3] = {1.0f, 0.0f, 0.0f};

  if (fabsf(dot_v3v3(r_normal, guess)) > 0.99f) {
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
                                      float *r_radius)
{
  zero_v2(r_center);

  for (const CircleVert &cv : verts) {
    add_v2_v2(r_center, cv.co_2d);
  }
  if (!verts.is_empty()) {
    mul_v2_fl(r_center, 1.0f / verts.size());
  }

  double total_dist = 0.0;
  for (const CircleVert &cv : verts) {
    total_dist += len_v2v2(r_center, cv.co_2d);
  }

  if (!verts.is_empty()) {
    *r_radius = (float)(total_dist / verts.size());
  }
  else {
    *r_radius = 0.0f;
  }
}

static void calculate_circle_inside_fit(const Vector<CircleVert> &verts,
                                        float r_center[2],
                                        float *r_radius)
{
  if (verts.is_empty()) {
    zero_v2(r_center);
    *r_radius = 0.0f;
    return;
  }

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
  if (verts.is_empty()) {
    return;
  }

  float vec[2];
  sub_v2_v2v2(vec, verts[0].co_2d, center);
  float start_angle = atan2f(vec[1], vec[0]);

  float total_angle = is_closed ? (2.0f * M_PI) : M_PI;

  int divisions = is_closed ? verts.size() : (verts.size() - 1);
  if (divisions < 1) {
    divisions = 1;
  }

  float step = total_angle / divisions;

  for (int i = 0; i < verts.size(); i++) {
    float angle;

    if (is_regular) {
      angle = start_angle + (step * i) + rotation_angle;
    }
    else {
      sub_v2_v2v2(vec, verts[i].co_2d, center);
      angle = atan2f(vec[1], vec[0]) + rotation_angle;
    }

    verts[i].target_2d[0] = center[0] + (cosf(angle) * radius);
    verts[i].target_2d[1] = center[1] + (sinf(angle) * radius);
  }
}

void bmo_circularize_exec(BMesh *bm, BMOperator *op)
{
  const float influence = BMO_slot_float_get(op->slots_in, "influence");

  const bool regular = BMO_slot_bool_get(op->slots_in, "regular");
  const int fit_method = BMO_slot_int_get(op->slots_in, "fit_method");
  const float custom_radius = BMO_slot_float_get(op->slots_in, "custom_radius");
  const float angle = BMO_slot_float_get(op->slots_in, "angle");

  Vector<LoopData> loops;
  get_input_loops(bm, loops);

  for (LoopData &loop_data : loops) {
    const Vector<BMVert *> &loop = loop_data.verts;

    if (loop.size() < 3) {
      continue;
    }

    float center_3d[3], normal[3], p[3], q[3];
    calculate_plane_basis(loop, center_3d, normal, p, q);

    Vector<CircleVert> circle_verts;
    project_loop_to_2d(loop, center_3d, p, q, circle_verts);

    float circle_center_2d[2];
    float radius;

    if (fit_method == 1) {
      calculate_circle_inside_fit(circle_verts, circle_center_2d, &radius);
    }
    else {
      calculate_circle_best_fit(circle_verts, circle_center_2d, &radius);
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

      interp_v3_v3v3(cv.v->co, cv.v->co, final_pos, influence);
    }
  }
}
