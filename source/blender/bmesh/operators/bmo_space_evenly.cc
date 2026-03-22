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
 * Walk from start_edge in both directions and return the resulting vertex chain.
 */
static SpaceChainData walk_edges(BMEdge *start_edge, Set<BMEdge *> &r_visited)
{
  SpaceChainData chain_data;
  Set<BMVert *> visited_verts;

  chain_data.verts.append(start_edge->v1);
  chain_data.verts.append(start_edge->v2);
  visited_verts.add(start_edge->v1);
  visited_verts.add(start_edge->v2);
  r_visited.add(start_edge);

  auto walk_fn = [&](BMVert *curr_v, Vector<BMVert *> &list) {
    while (true) {
      BMEdge *next_e = nullptr;
      BMIter eiter;
      BMEdge *e_candidate;
      BM_ITER_ELEM (e_candidate, &eiter, curr_v, BM_EDGES_OF_VERT) {
        if (!r_visited.contains(e_candidate) && BM_elem_flag_test(e_candidate, BM_ELEM_TAG)) {
          next_e = e_candidate;
          break;
        }
      }
      if (!next_e) {
        break;
      }
      BMVert *next_v = BM_edge_other_vert(next_e, curr_v);
      if (visited_verts.contains(next_v)) {
        break;
      }
      curr_v = next_v;
      visited_verts.add(curr_v);
      list.append(curr_v);
      r_visited.add(next_e);
    }
  };

  /* The initial edge direction (v1 -> v2) is arbitrary.
   * We walk from v2 to extend this sequence. */
  walk_fn(start_edge->v2, chain_data.verts);

  Vector<BMVert *> pre_chain;
  walk_fn(start_edge->v1, pre_chain);

  if (!pre_chain.is_empty()) {
    std::reverse(pre_chain.begin(), pre_chain.end());
    pre_chain.extend(chain_data.verts);
    chain_data.verts = std::move(pre_chain);
  }

  BMEdge *closing_edge = BM_edge_exists(chain_data.verts.first(), chain_data.verts.last());
  if (closing_edge && BM_elem_flag_test(closing_edge, BM_ELEM_TAG)) {
    if (!r_visited.contains(closing_edge)) {
      r_visited.add(closing_edge);
    }
    chain_data.is_closed = true;
  }
  else {
    chain_data.is_closed = false;
  }

  return chain_data;
}

/**
 * Build vertex chains from selected edges.
 */
static void get_space_input_chains(BMesh *bm, Vector<SpaceChainData> &r_chains)
{
  Set<BMEdge *> visited;
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
        r_chains.append(std::move(chain));
      }
    }
  }
}

/**
 * Compute cumulative distances along the chain and the corresponding
 * evenly spaced target distances.
 */
static SpaceMeasurements measure_chain(const SpaceChainData &chain)
{
  SpaceMeasurements measure;
  const int num_verts = chain.verts.size();

  /* Measure cumulative distances. */
  float current_dist = 0.0f;
  measure.knot_distances.reserve(num_verts + (chain.is_closed ? 1 : 0));
  /* The very first vertex is at distance 0. */
  measure.knot_distances.append(0.0f);

  for (const int i : IndexRange(num_verts).drop_front(1)) {
    current_dist += math::distance(float3(chain.verts[i]->co), float3(chain.verts[i - 1]->co));
    measure.knot_distances.append(current_dist);
  }
  /* The for loop missed the final gap if its a closed chain. */
  if (chain.is_closed) {
    current_dist += math::distance(float3(chain.verts.last()->co),
                                   float3(chain.verts.first()->co));
    measure.knot_distances.append(current_dist);
  }
  measure.total_length = current_dist;

  /* Generally, for any given number of n points, there's always n-1 piecewise cubic spline
   * equations but for a closed chain, the last vertex will connect back to the first so there's n
   * cubic spline equations in that case. */
  const int num_segments = chain.is_closed ? num_verts : num_verts - 1;
  float step = measure.total_length / float(num_segments);
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
  for (const int i : IndexRange(n - 1)) {
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
  for (const int i : IndexRange(1, n - 2)) {
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

/**
 * Compute cubic spline coefficients for one coordinate axis.
 *
 * For open chains this solves the tridiagonal system directly.
 * For closed chains, 4 vertices from the end are copied before the start and 4 from
 * the start after the end, making the closed loop appear open to the Thomas algorithm.
 */
static void calculate_splines_axis(Span<float> unique_distances,
                                   Span<float> unique_coords,
                                   bool is_closed,
                                   float total_length,
                                   Vector<SplineCoeffs> &r_coeffs)
{
  int num_points = unique_coords.size();
  if (is_closed) {
    Vector<float> padded_coords;
    Vector<float> padded_distances;
    float dist_start = unique_distances[0];
    float running_dt = 0.0f;

    for (const int k : IndexRange(1, 4)) {
      int coord_index = mod_i(num_points - k, num_points);
      padded_coords.insert(0, unique_coords[coord_index]);

      int knot_index_curr = mod_i(num_points - k + 1, num_points);
      int knot_index_prev = mod_i(num_points - k, num_points);
      float segment_length;
      if (knot_index_curr == 0) {
        segment_length = total_length - unique_distances[num_points - 1];
      }
      else {
        segment_length = unique_distances[knot_index_curr] - unique_distances[knot_index_prev];
      }
      running_dt += segment_length;
      padded_distances.insert(0, dist_start - running_dt);
    }
    padded_coords.extend(unique_coords);
    padded_distances.extend(unique_distances);
    padded_coords.append(unique_coords[0]);
    padded_distances.append(total_length);

    running_dt = 0.0f;
    for (const int k : IndexRange(4)) {
      int coord_index = mod_i(k + 1, num_points);
      padded_coords.append(unique_coords[coord_index]);

      int knot_index_curr = mod_i(k + 1, num_points);
      int knot_index_prev = mod_i(k, num_points);
      float segment_length;
      if (knot_index_curr == 0) {
        segment_length = total_length - unique_distances[num_points - 1];
      }
      else {
        segment_length = unique_distances[knot_index_curr] - unique_distances[knot_index_prev];
      }

      running_dt += segment_length;
      padded_distances.append(total_length + running_dt);
    }

    Vector<SplineCoeffs> all_coeffs;
    solve_thomas_algorithm(padded_distances, padded_coords, all_coeffs);

    if (all_coeffs.size() > 8) {
      for (const int i : IndexRange(4, all_coeffs.size() - 8)) {
        r_coeffs.append(all_coeffs[i]);
      }
    }
  }
  else {
    solve_thomas_algorithm(unique_distances, unique_coords, r_coeffs);
  }
}

/** Return the index of the spline segment that contains target_distance. */
static int find_spline_segment(Span<float> knot_distances, float target_distance)
{
  for (const int k : IndexRange(knot_distances.size() - 1)) {
    if (target_distance >= knot_distances[k] && target_distance <= knot_distances[k + 1]) {
      return k;
    }
  }
  BLI_assert_unreachable();
  return knot_distances.size() - 2;
}

/** Evaluate linear interpolation at target_distance. */
static float3 evaluate_linear(Span<float> tknots,
                              Span<float> coords_x,
                              Span<float> coords_y,
                              Span<float> coords_z,
                              float target_distance)
{
  int segment = find_spline_segment(tknots, target_distance);
  float seg_start = tknots[segment];
  float seg_end = tknots[segment + 1];
  float blend = (target_distance - seg_start) / (seg_end - seg_start);
  int next_knot = mod_i(segment + 1, coords_x.size());
  float3 start_pos(coords_x[segment], coords_y[segment], coords_z[segment]);
  float3 end_pos(coords_x[next_knot], coords_y[next_knot], coords_z[next_knot]);
  return math::interpolate(start_pos, end_pos, blend);
}

/** Evaluates the cubic spline at target_distance. */
static float3 evaluate_cubic(Span<float> tknots,
                             Span<SplineCoeffs> coeffs_x,
                             Span<SplineCoeffs> coeffs_y,
                             Span<SplineCoeffs> coeffs_z,
                             float target_distance)
{
  int segment = find_spline_segment(tknots, target_distance);
  float dt = target_distance - coeffs_x[segment].x;
  float dt2 = dt * dt;
  float dt3 = dt2 * dt;
  const SplineCoeffs &cx = coeffs_x[segment];
  const SplineCoeffs &cy = coeffs_y[segment];
  const SplineCoeffs &cz = coeffs_z[segment];
  return float3(cx.a + cx.b * dt + cx.c * dt2 + cx.d * dt3,
                cy.a + cy.b * dt + cy.c * dt2 + cy.d * dt3,
                cz.a + cz.b * dt + cz.c * dt2 + cz.d * dt3);
}

void bmo_space_evenly_exec(BMesh *bm, BMOperator *op)
{
  const float influence = BMO_slot_float_get(op->slots_in, "factor");
  const int interpolation = BMO_slot_int_get(op->slots_in, "interpolation");
  const bool lock_x = BMO_slot_bool_get(op->slots_in, "lock_x");
  const bool lock_y = BMO_slot_bool_get(op->slots_in, "lock_y");
  const bool lock_z = BMO_slot_bool_get(op->slots_in, "lock_z");

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(
      bm, op->slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  Vector<SpaceChainData> chains;
  get_space_input_chains(bm, chains);

  for (SpaceChainData &chain : chains) {
    SpaceMeasurements measure = measure_chain(chain);

    Vector<float> coords_x, coords_y, coords_z;
    for (BMVert *v : chain.verts) {
      coords_x.append(v->co[0]);
      coords_y.append(v->co[1]);
      coords_z.append(v->co[2]);
    }
    Vector<SplineCoeffs> coeffs_x, coeffs_y, coeffs_z;
    if (interpolation == Cubic) {
      Span<float> unique_dists = chain.is_closed ?
                                     Span<float>(measure.knot_distances).drop_back(1) :
                                     Span<float>(measure.knot_distances);
      calculate_splines_axis(
          unique_dists, coords_x, chain.is_closed, measure.total_length, coeffs_x);
      calculate_splines_axis(
          unique_dists, coords_y, chain.is_closed, measure.total_length, coeffs_y);
      calculate_splines_axis(
          unique_dists, coords_z, chain.is_closed, measure.total_length, coeffs_z);
    }

    for (const int i : IndexRange(chain.verts.size())) {
      /* The first and last vertices of an open chain are anchor points so they are skipped. */
      if (!chain.is_closed && (i == 0 || i == chain.verts.size() - 1)) {
        continue;
      }

      float target_distance = measure.spaced_distances[i];
      float3 new_pos;

      if (interpolation == Linear) {
        new_pos = evaluate_linear(
            measure.knot_distances, coords_x, coords_y, coords_z, target_distance);
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
      chain.verts[i]->co[0] = final_pos.x;
      chain.verts[i]->co[1] = final_pos.y;
      chain.verts[i]->co[2] = final_pos.z;
    }
  }
}

}  // namespace blender
