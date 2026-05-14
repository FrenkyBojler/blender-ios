/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Distributes vertices evenly along an edge.
 */
#include "BLI_math_geom.h"
#include "BLI_math_solvers.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */
#include <optional>

namespace blender {

/** Used as a threshold to decide if all vertices are at the same position. */
constexpr float STACKED_THRESHOLD = 1e-6f;
/** Epsilon to prevent zero division. */
constexpr float SPACE_EPSILON = 1e-8f;

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
 * Returns std::nullopt when all vertices are at the same position.
 */
static std::optional<SpaceChainData> walk_edges(BMEdge *start_edge, Set<BMEdge *> &r_visited)
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
    std::ranges::reverse(pre_chain);
    pre_chain.extend(chain_data.verts);
    chain_data.verts = std::move(pre_chain);
  }

  /* Skip chains where all vertices are at the same location. */
  bool all_stacked = true;
  for (const int i : chain_data.verts.index_range().drop_back(1)) {
    if (math::distance(float3(chain_data.verts[i]->co), float3(chain_data.verts[i + 1]->co)) >
        STACKED_THRESHOLD)
    {
      all_stacked = false;
      break;
    }
  }
  if (all_stacked) {
    return std::nullopt;
  }

  BMEdge *closing_edge = BM_edge_exists(chain_data.verts.first(), chain_data.verts.last());
  if (closing_edge && BM_elem_flag_test(closing_edge, BM_ELEM_TAG)) {
    r_visited.add(closing_edge);
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
    std::optional<SpaceChainData> chain = walk_edges(edge, visited);
    if (chain) {
      r_chains.append(std::move(*chain));
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
  measure.knot_distances.resize(num_verts + (chain.is_closed ? 1 : 0));
  /* The very first vertex is at distance 0. */
  measure.knot_distances[0] = 0.0f;

  for (const int i : IndexRange(num_verts).drop_front(1)) {
    current_dist += math::distance(float3(chain.verts[i]->co), float3(chain.verts[i - 1]->co));
    measure.knot_distances[i] = current_dist;
  }
  /* The for loop missed the final gap if its a closed chain. */
  if (chain.is_closed) {
    current_dist += math::distance(float3(chain.verts.last()->co),
                                   float3(chain.verts.first()->co));
    measure.knot_distances.last() = current_dist;
  }
  measure.total_length = current_dist;

  /* Generally, for any given number of n points, there's always n-1 piecewise cubic spline
   * equations but for a closed chain, the last vertex will connect back to the first so there's n
   * cubic spline equations in that case. */
  const int num_segments = chain.is_closed ? num_verts : num_verts - 1;
  float step = measure.total_length / float(num_segments);
  measure.spaced_distances.resize(num_verts);

  for (const int i : IndexRange(num_verts)) {
    measure.spaced_distances[i] = i * step;
  }

  return measure;
}

/**
 * Compute cubic spline coefficients for one coordinate axis.
 */
static void calculate_splines_axis(Span<float> distances,
                                   Span<float> coords,
                                   bool is_closed,
                                   float total_length,
                                   Vector<SplineCoeffs> &r_coeffs)
{
  const int num_verts = coords.size();
  if (num_verts < 2) {
    return;
  }
  const int num_segments = is_closed ? num_verts : num_verts - 1;
  Array<float> segment_length(num_segments);

  for (const int i : IndexRange(num_segments)) {
    segment_length[i] = (is_closed && i == num_verts - 1) ?
                            total_length - distances[num_verts - 1] :
                            distances[i + 1] - distances[i];
    if (segment_length[i] == 0.0f) {
      segment_length[i] = SPACE_EPSILON;
    }
  }

  /* Stores second derivative coefficients. For a natural cubic spline, the boundary
   * condition defines the first and last points as zero. */
  Array<float> c_vals(num_verts, 0.0f);

  /* The Thomas algorithm used in `BLI_tridiagonal_solve` can't properly solve
   * a cyclic tridiagonal system so in this case, we use the Sherman-Morrison formula
   * via `BLI_tridiagonal_solve_cyclic`. */
  if (is_closed) {
    Array<float> lower_diag(num_verts), diag(num_verts), upper_diag(num_verts), rhs(num_verts);
    for (const int i : IndexRange(num_verts)) {
      const int v_prev = mod_i(i - 1, num_verts);
      const int v_next = mod_i(i + 1, num_verts);
      lower_diag[i] = segment_length[v_prev];
      diag[i] = 2.0f * (segment_length[v_prev] + segment_length[i]);
      upper_diag[i] = segment_length[i];
      rhs[i] = 3.0f * (((coords[v_next] - coords[i]) / segment_length[i]) -
                       ((coords[i] - coords[v_prev]) / segment_length[v_prev]));
    }
    BLI_tridiagonal_solve_cyclic(
        lower_diag.data(), diag.data(), upper_diag.data(), rhs.data(), c_vals.data(), num_verts);
  }
  else {
    /* For a natural cubic spline the curvature at the first and last point
     * is 0, so for n given points, we only have n-2 unknown interior points. */
    const int interior = num_verts - 2;
    Array<float> lower_diag(interior), diag(interior), upper_diag(interior), rhs(interior);

    for (const int i : IndexRange(interior)) {
      const int v_index = i + 1;
      lower_diag[i] = segment_length[v_index - 1];
      diag[i] = 2.0f * (segment_length[v_index - 1] + segment_length[v_index]);
      upper_diag[i] = segment_length[v_index];
      rhs[i] = 3.0f * (((coords[v_index + 1] - coords[v_index]) / segment_length[v_index]) -
                       ((coords[v_index] - coords[v_index - 1]) / segment_length[v_index - 1]));
    }
    BLI_tridiagonal_solve(lower_diag.data(),
                          diag.data(),
                          upper_diag.data(),
                          rhs.data(),
                          c_vals.data() + 1,
                          interior);
  }

  /* Build polynomial coefficients for each segment. */
  for (const int i : IndexRange(num_segments)) {
    const int v_next = is_closed ? mod_i(i + 1, num_verts) : i + 1;

    const float coeff_a = coords[i];
    const float coeff_b = ((coords[v_next] - coords[i]) / segment_length[i]) -
                          (segment_length[i] * (c_vals[v_next] + 2.0f * c_vals[i])) / 3.0f;
    const float coeff_c = c_vals[i];
    const float coeff_d = (c_vals[v_next] - c_vals[i]) / (3.0f * segment_length[i]);
    r_coeffs.append({coeff_a, coeff_b, coeff_c, coeff_d, distances[i]});
  }
}

/** Return the index of the spline segment that contains target_distance. */
static int find_spline_segment(Span<float> knot_distances, float target_distance)
{
  auto upper_knot = std::upper_bound(
      knot_distances.begin(), knot_distances.end(), target_distance);
  int segment_index = std::distance(knot_distances.begin(), upper_knot) - 1;
  return std::clamp(segment_index, 0, int(knot_distances.size()) - 2);
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
  float denom = seg_end - seg_start;
  float factor = denom > 0 ? (target_distance - seg_start) / denom : 0.0f;
  int next_knot = mod_i(segment + 1, coords_x.size());
  float3 start_pos(coords_x[segment], coords_y[segment], coords_z[segment]);
  float3 end_pos(coords_x[next_knot], coords_y[next_knot], coords_z[next_knot]);
  return math::interpolate(start_pos, end_pos, factor);
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

  const SplineCoeffs &cx = coeffs_x[segment];
  const SplineCoeffs &cy = coeffs_y[segment];
  const SplineCoeffs &cz = coeffs_z[segment];

  return float3(cx.a + dt * (cx.b + dt * (cx.c + dt * cx.d)),
                cy.a + dt * (cy.b + dt * (cy.c + dt * cy.d)),
                cz.a + dt * (cz.b + dt * (cz.c + dt * cz.d)));
}

void bmo_space_evenly_exec(BMesh *bm, BMOperator *op)
{
  const float influence = BMO_slot_float_get(op->slots_in, "factor");
  const InterpolationMethod interpolation = static_cast<InterpolationMethod>(
      BMO_slot_int_get(op->slots_in, "interpolation"));
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
      calculate_splines_axis(
          measure.knot_distances, coords_x, chain.is_closed, measure.total_length, coeffs_x);
      calculate_splines_axis(
          measure.knot_distances, coords_y, chain.is_closed, measure.total_length, coeffs_y);
      calculate_splines_axis(
          measure.knot_distances, coords_z, chain.is_closed, measure.total_length, coeffs_z);
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
      copy_v3_v3(chain.verts[i]->co, final_pos);
    }
  }
}

}  // namespace blender
