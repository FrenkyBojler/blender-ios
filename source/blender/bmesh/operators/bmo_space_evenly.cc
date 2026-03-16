/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Distributes vertices evenly along an edge.
 */
#include "BLI_math_geom.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

/**
 * A chain of vertices collected from a walk along connected edges.
 */
struct SpaceChainData {
  /** Ordered vertices from one end of the chain to the other. */
  Vector<BMVert *> verts;
  /** True if the path forms a closed ring. */
  bool is_closed;
};

/**
 * Stores measured and target distances for a vertex chain.
 */
struct SpaceMeasurements {
  /** Cumulative distances along the chain. */
  Vector<float> knot_distances;
  /** Evenly spaced target distances along the chain. */
  Vector<float> spaced_distances;
  /** The total length of the chain. */
  float total_length;
};

/**
 * Coefficients for the cubic spline curve equation.
 */
struct SplineCoeffs {
  /** Value at the start of the segment. */
  float a;
  /** First-order coefficient. */
  float b;
  /** Second-order coefficient. */
  float c;
  /** Third-order coefficient. */
  float d;
  /** Parameter value at the start of the segment. */
  float x;
};

/** Interpolation method used for spacing vertices. */
enum InterpolationMethod {
  /** Fit a smooth curve using natural cubic splines. */
  Cubic = 0,
  /** Interpolates linearly between consecutive vertices. */
  Linear = 1,
};

/**
 * Extracts the parallel vertex chain across a quad strip.
 * Given an edge loop selection and a starting face, this
 * returns the immediate adjacent parallel loop.
 */
static bool extract_parallel_chain(const SpaceChainData &base,
                                   BMFace *start_face,
                                   SpaceChainData &r_parallel)
{
  if (base.verts.size() < 2 || start_face->len != 4) {
    return false;
  }

  BMFace *current_face = start_face;
  r_parallel.is_closed = base.is_closed;
  /* A closed chain of vertices will have the last vertex connecting back to the
   * first one so the number of segments between vertices is the same as the total
   * number of vertices. */
  int num_segments = base.is_closed ? base.verts.size() : base.verts.size() - 1;

  for (const int i : IndexRange(num_segments)) {
    BMVert *v_curr = base.verts[i];
    BMVert *v_next = base.verts[mod_i(i + 1, base.verts.size())];

    BMEdge *base_edge = BM_edge_exists(v_curr, v_next);
    BMLoop *selection_loop = BM_face_edge_share_loop(current_face, base_edge);

    BMVert *opp_curr, *opp_next;
    BMEdge *side_edge;

    if (selection_loop->v == v_curr) {
      opp_curr = selection_loop->prev->v;
      opp_next = selection_loop->next->next->v;
      side_edge = selection_loop->next->e;
    }
    else {
      opp_curr = selection_loop->next->next->v;
      opp_next = selection_loop->prev->v;
      side_edge = selection_loop->prev->e;
    }

    if (i == 0) {
      r_parallel.verts.append(opp_curr);
    }
    r_parallel.verts.append(opp_next);

    if (i < num_segments - 1) {
      BMVert *v_after = base.verts[mod_i(i + 2, base.verts.size())];
      BMEdge *next_base_edge = BM_edge_exists(v_next, v_after);
      BMFace *next_face = nullptr;
      BMIter fiter;
      BMFace *f;
      BM_ITER_ELEM (f, &fiter, next_base_edge, BM_FACES_OF_EDGE) {
        if (f->len == 4 && f != current_face && BM_face_edge_share_loop(f, side_edge)) {
          next_face = f;
          break;
        }
      }
      if (!next_face) {
        return false;
      }
      current_face = next_face;
    }
  }

  return true;
}

/**
 * Follows quad strips starting from the initial selection to gather all parallel loops.
 * The process continues until the strip is interrupted by a mesh boundary, irregular
 * topology(triangles or ngons in this case), or when the search cycles back
 * to a processed vertex.
 */
static void expand_parallel_chains(const SpaceChainData &base_chain,
                                   BMFace *start_face,
                                   Set<BMVert *> &visited_verts,
                                   Vector<SpaceChainData> &r_all_chains)
{
  SpaceChainData current_chain = base_chain;
  BMFace *current_face = start_face;

  while (true) {
    SpaceChainData parallel_chain;
    if (!extract_parallel_chain(current_chain, current_face, parallel_chain)) {
      break;
    }

    if (visited_verts.contains(parallel_chain.verts[0])) {
      break;
    }

    for (BMVert *v : parallel_chain.verts) {
      visited_verts.add(v);
    }
    r_all_chains.append(parallel_chain);
    BMEdge *first_parallel_edge = BM_edge_exists(parallel_chain.verts[0], parallel_chain.verts[1]);
    if (!first_parallel_edge) {
      break;
    }

    BMFace *next_face = nullptr;
    BMIter fiter;
    BMFace *f;
    BM_ITER_ELEM (f, &fiter, first_parallel_edge, BM_FACES_OF_EDGE) {
      if (f != current_face) {
        next_face = f;
        break;
      }
    }
    if (!next_face) {
      break;
    }
    current_chain = std::move(parallel_chain);
    current_face = next_face;
  }
}

/**
 * Return the next unvisited candidate edge connected to v,
 * preferring the edge that continues e_prev most directly.
 */
static BMEdge *get_next_space_edge(BMVert *v, BMEdge *e_prev, Set<BMEdge *> &r_visited)
{
  BMEdge *best_edge = nullptr;
  float best_straightness = -1.0f;
  BMVert *vert_behind = BM_edge_other_vert(e_prev, v);
  float3 incoming_dir = math::normalize(float3(v->co) - float3(vert_behind->co));

  BMIter eiter;
  BMEdge *e_next;
  BM_ITER_ELEM (e_next, &eiter, v, BM_EDGES_OF_VERT) {
    if (r_visited.contains(e_next)) {
      continue;
    }

    if (BM_elem_flag_test(e_next, BM_ELEM_TAG)) {
      BMVert *vert_ahead = BM_edge_other_vert(e_next, v);
      float3 outgoing_dir = math::normalize(float3(vert_ahead->co) - float3(v->co));
      float straightness = math::dot(incoming_dir, outgoing_dir);

      if (straightness > best_straightness) {
        best_straightness = straightness;
        best_edge = e_next;
      }
    }
  }
  return best_edge;
}

/**
 * Walk from start_edge in both directions and return the resulting vertex chain.
 */
static SpaceChainData walk_edges(BMEdge *start_edge, Set<BMEdge *> &r_visited)
{
  SpaceChainData chain_data;
  chain_data.verts.append(start_edge->v1);
  chain_data.verts.append(start_edge->v2);
  r_visited.add(start_edge);

  auto walk_fn = [&](BMVert *curr_v, BMEdge *curr_e, Vector<BMVert *> &list) {
    while (true) {
      BMEdge *next_e = get_next_space_edge(curr_v, curr_e, r_visited);
      if (!next_e) {
        break;
      }
      curr_v = BM_edge_other_vert(next_e, curr_v);
      curr_e = next_e;
      list.append(curr_v);
      r_visited.add(curr_e);
    }
  };

  /* The initial edge direction (v1 -> v2) is arbitrary.
   * We walk from v2 to extend this sequence. */
  walk_fn(start_edge->v2, start_edge, chain_data.verts);

  if (chain_data.verts.size() > 2 && chain_data.verts.first() == chain_data.verts.last()) {
    chain_data.verts.remove_last();
    chain_data.is_closed = true;
    return chain_data;
  }

  Vector<BMVert *> pre_chain;
  walk_fn(start_edge->v1, start_edge, pre_chain);

  if (!pre_chain.is_empty()) {
    std::reverse(pre_chain.begin(), pre_chain.end());
    pre_chain.extend(chain_data.verts);
    chain_data.verts = std::move(pre_chain);
  }

  chain_data.is_closed = false;
  return chain_data;
}

/**
 * Construct vertex chains from candidate edges.
 * If use_parallel is true, candidates are collected by flood-filling
 * across opposite edges of even-sided faces. Otherwise, only tagged edges are used.
 */
static void get_space_input_chains(BMesh *bm, bool use_parallel, Vector<SpaceChainData> &r_chains)
{
  Set<BMEdge *> visited;
  Vector<SpaceChainData> base_chains;
  BMIter iter;
  BMEdge *edge;
  BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
    if (!BM_elem_flag_test(edge, BM_ELEM_TAG) || visited.contains(edge)) {
      continue;
    }
    SpaceChainData chain = walk_edges(edge, visited);

    /* Skip chains where all vertices are at the same location. */
    if (chain.verts.size() >= 3) {
      bool all_stacked = true;
      for (const int i : IndexRange(chain.verts.size()).drop_front(1)) {
        if (math::distance(float3(chain.verts[0]->co), float3(chain.verts[i]->co)) > 1e-6f) {
          all_stacked = false;
          break;
        }
      }
      if (!all_stacked) {
        base_chains.append(std::move(chain));
      }
    }
  }

  for (const SpaceChainData &chain : base_chains) {
    r_chains.append(chain);
  }

  if (!use_parallel) {
    return;
  }

  Set<BMVert *> global_visited_parallel;
  for (const SpaceChainData &base_chain : base_chains) {
    for (BMVert *v : base_chain.verts) {
      global_visited_parallel.add(v);
    }
  }

  for (const SpaceChainData &base_chain : base_chains) {
    BMEdge *first_edge = BM_edge_exists(base_chain.verts[0], base_chain.verts[1]);
    if (!first_edge) {
      continue;
    }

    BMIter fiter;
    BMFace *f;
    BM_ITER_ELEM (f, &fiter, first_edge, BM_FACES_OF_EDGE) {
      expand_parallel_chains(base_chain, f, global_visited_parallel, r_chains);
    }
  }
}

/**
 * Compute cumulative distances along the chain and the corresponding
 * evenly spaced target distances.
 */
static SpaceMeasurements measure_chain(const SpaceChainData &loop)
{
  SpaceMeasurements measure;
  const int num_verts = loop.verts.size();

  /* Measure cumulative distances. */
  float current_dist = 0.0f;
  measure.knot_distances.reserve(num_verts + (loop.is_closed ? 1 : 0));
  /* The very first vertex is at distance 0. */
  measure.knot_distances.append(0.0f);

  for (int i = 1; i < num_verts; i++) {
    current_dist += len_v3v3(loop.verts[i]->co, loop.verts[i - 1]->co);
    measure.knot_distances.append(current_dist);
  }
  /* The for loop missed the final gap if its a closed loop. */
  if (loop.is_closed) {
    current_dist += len_v3v3(loop.verts.last()->co, loop.verts.first()->co);
    measure.knot_distances.append(current_dist);
  }
  measure.total_length = current_dist;

  /* Generally, for any given number of n points, there's always n-1 piecewise cubic spline
   * equations but for a closed chain, the last vertex will connect back to the first so there's n
   * cubic spline equations in that case. */
  const int num_segments = loop.is_closed ? num_verts : num_verts - 1;

  float step = measure.total_length / (float)num_segments;
  measure.spaced_distances.reserve(num_verts);

  for (const int i : IndexRange(num_verts)) {
    measure.spaced_distances.append(i * step);
  }

  return measure;
}

/**
 * Solves a tridiagonal linear system using the Thomas Algorithm to find
 * coefficients for a natural cubic spline.
 */
static void solve_thomas_algorithm(Span<float> t, Span<float> y, Vector<SplineCoeffs> &r_coeffs)
{
  int n = t.size();
  if (n < 2) {
    return;
  }
  /* Parameter interval between consecutive knots. */
  Vector<float> h(n - 1);
  /* Forward elimination variables. */
  Vector<float> l(n);
  Vector<float> u(n);
  Vector<float> z(n);
  /* The final polynomial coefficients. */
  Vector<float> c(n);
  Vector<float> b(n);
  Vector<float> d(n);

  /* Calculate the length of each segment between consecutive knots. */
  for (int i = 0; i < n - 1; i++) {
    h[i] = t[i + 1] - t[i];
    /* In the case where there are two overlapping verticies, we give an arbitrary length
     * to prevent a zero division. */
    if (h[i] == 0.0f) {
      h[i] = 1e-8f;
    }
  }

  /* Boundary conditions. */
  l[0] = 1.0f;
  u[0] = 0.0f;
  z[0] = 0.0f;

  /* Forward Elimination. */
  for (int i = 1; i < n - 1; i++) {
    float q = (3.0f / h[i]) * (y[i + 1] - y[i]) - (3.0f / h[i - 1]) * (y[i] - y[i - 1]);
    l[i] = 2.0f * (t[i + 1] - t[i - 1]) - h[i - 1] * u[i - 1];
    if (l[i] == 0.0f) {
      l[i] = 1e-8f;
    }
    u[i] = h[i] / l[i];
    z[i] = (q - h[i - 1] * z[i - 1]) / l[i];
  }
  /* End boundary condition. */
  l[n - 1] = 1.0f;
  z[n - 1] = 0.0f;
  c[n - 1] = 0.0f;

  /* Backsubstitution. */
  for (int i = n - 2; i >= 0; i--) {
    c[i] = z[i] - u[i] * c[i + 1];
    b[i] = (y[i + 1] - y[i]) / h[i] - h[i] * (c[i + 1] + 2.0f * c[i]) / 3.0f;
    d[i] = (c[i + 1] - c[i]) / (3.0f * h[i]);
  }

  /* Build spline coefficients for each segment. */
  for (const int i : IndexRange(n - 1)) {
    r_coeffs.append({y[i], b[i], c[i], d[i], t[i]});
  }
}

static void calculate_splines_axis(Span<float> distances,
                                   Span<float> coordinates,
                                   Vector<SplineCoeffs> &r_coeffs)
{
  /* Todo: Add closed chain spline support. */
  solve_thomas_algorithm(distances, coordinates, r_coeffs);
}

/** Return the index of the spline segment that contains target_distance. */
static int find_spline_segment(Span<float> knot_distances, float target_distance)
{
  for (int k = 0; k < knot_distances.size() - 1; k++) {
    if (target_distance >= knot_distances[k] && target_distance <= knot_distances[k + 1]) {
      return k;
    }
  }
  BLI_assert_unreachable();
  return knot_distances.size() - 2;
}

/** Evaluate linear interpolation at target_distance. */
static float3 evaluate_linear(Span<float> tknots,
                              Span<float> solver_x,
                              Span<float> solver_y,
                              Span<float> solver_z,
                              float target_distance)
{
  int n = find_spline_segment(tknots, target_distance);
  float t_start = tknots[n];
  float t_end = tknots[n + 1];
  float t_factor = 0.0f;
  if (t_end - t_start > 1e-6f) {
    t_factor = (target_distance - t_start) / (t_end - t_start);
  }
  float3 p1(solver_x[n], solver_y[n], solver_z[n]);
  float3 p2(solver_x[n + 1], solver_y[n + 1], solver_z[n + 1]);
  return math::interpolate(p1, p2, t_factor);
}

/** Evaluates the cubic spline at target_distance. */
static float3 evaluate_cubic(Span<float> tknots,
                             Span<SplineCoeffs> coeffs_x,
                             Span<SplineCoeffs> coeffs_y,
                             Span<SplineCoeffs> coeffs_z,
                             float target_distance)
{
  int n = find_spline_segment(tknots, target_distance);
  float dt = target_distance - coeffs_x[n].x;
  float dt2 = dt * dt;
  float dt3 = dt2 * dt;
  return float3(coeffs_x[n].a + coeffs_x[n].b * dt + coeffs_x[n].c * dt2 + coeffs_x[n].d * dt3,
                coeffs_y[n].a + coeffs_y[n].b * dt + coeffs_y[n].c * dt2 + coeffs_y[n].d * dt3,
                coeffs_z[n].a + coeffs_z[n].b * dt + coeffs_z[n].c * dt2 + coeffs_z[n].d * dt3);
}

void bmo_space_evenly_exec(BMesh *bm, BMOperator *op)
{
  const float influence = BMO_slot_float_get(op->slots_in, "factor");
  const int interpolation = BMO_slot_int_get(op->slots_in, "interpolation");
  const bool use_parallel = BMO_slot_bool_get(op->slots_in, "use_parallel");
  const bool lock_x = BMO_slot_bool_get(op->slots_in, "lock_x");
  const bool lock_y = BMO_slot_bool_get(op->slots_in, "lock_y");
  const bool lock_z = BMO_slot_bool_get(op->slots_in, "lock_z");

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(
      bm, op->slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  Vector<SpaceChainData> chains;
  get_space_input_chains(bm, use_parallel, chains);

  for (SpaceChainData &chain : chains) {
    SpaceMeasurements measure = measure_chain(chain);

    Vector<float> solver_x, solver_y, solver_z;
    for (BMVert *v : chain.verts) {
      solver_x.append(v->co[0]);
      solver_y.append(v->co[1]);
      solver_z.append(v->co[2]);
    }
    if (chain.is_closed) {
      solver_x.append(chain.verts[0]->co[0]);
      solver_y.append(chain.verts[0]->co[1]);
      solver_z.append(chain.verts[0]->co[2]);
    }

    Vector<SplineCoeffs> coeffs_x, coeffs_y, coeffs_z;
    if (interpolation == Cubic) {
      calculate_splines_axis(measure.knot_distances, solver_x, coeffs_x);
      calculate_splines_axis(measure.knot_distances, solver_y, coeffs_y);
      calculate_splines_axis(measure.knot_distances, solver_z, coeffs_z);
    }

    for (int i = 0; i < chain.verts.size(); i++) {
      /* The first and last vertices of an open chain are anchor points so they are skipped. */
      if (!chain.is_closed && (i == 0 || i == chain.verts.size() - 1)) {
        continue;
      }

      float target_distance = measure.spaced_distances[i];
      float3 new_pos;

      if (interpolation == Linear) {
        new_pos = evaluate_linear(
            measure.knot_distances, solver_x, solver_y, solver_z, target_distance);
      }
      else {
        new_pos = evaluate_cubic(
            measure.knot_distances, coeffs_x, coeffs_y, coeffs_z, target_distance);
      }

      if (lock_x) {
        new_pos.x = chain.verts[i]->co[0];
      }
      if (lock_y) {
        new_pos.y = chain.verts[i]->co[1];
      }
      if (lock_z) {
        new_pos.z = chain.verts[i]->co[2];
      }

      float3 final_pos = math::interpolate(float3(chain.verts[i]->co), new_pos, influence);
      copy_v3_v3(chain.verts[i]->co, final_pos);
    }
  }
}

}  // namespace blender
