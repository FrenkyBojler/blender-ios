/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Distributes vertices evenly along one or more edge loops.
 * Endpoints of edge loops are not modified unless the loop is
 * cyclic.
 *
 * Vertices along the edge loop are redistributed to uniform spacing
 * based on the cumulative length of the edge loop and are interpolated
 * either smoothly via a natural cubic spline or linearly.
 */
#include <optional>

#include "BLI_math_vector.hh"

#include "BLI_binary_search.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_math_geom.h"
#include "BLI_math_solvers.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

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
  bool is_closed = false;
};

/**
 * Stores measured and target distances for a vertex chain.
 */
struct SpaceMeasurements {
  /** 3D coordinates of the vertices. */
  Array<float3> positions;
  /** Cumulative distances along the chain. */
  Array<float> knot_distances;
  /** The total length of the chain. */
  float total_length;
};

/**
 * Coefficients for the cubic spline curve equation, calculated per coordinate axis.
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

/**
 * Return the next tagged edge to walk from `v`, or null.
 * A null return will occur:
 * - When no other edge can be found.
 * - When there are 3+ connected edges (a logical "junction").
 */
static BMEdge *vert_next_walk_edge(BMVert *v, const Set<BMEdge *> &visited)
{
  BMEdge *e_next = nullptr;
  int tagged_count = 0;
  BMIter eiter;
  BMEdge *e;
  BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
    if (!BM_elem_flag_test(e, BM_ELEM_TAG)) {
      continue;
    }
    tagged_count++;
    if (tagged_count >= 3) {
      return nullptr;
    }
    if (!e_next && !visited.contains(e)) {
      e_next = e;
    }
  }
  return e_next;
}

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

  auto walk_fn = [&](BMVert *v_curr, Vector<BMVert *> &list) {
    while (true) {
      BMEdge *e_next = vert_next_walk_edge(v_curr, r_visited);
      if (!e_next) {
        break;
      }
      BMVert *v_next = BM_edge_other_vert(e_next, v_curr);
      if (visited_verts.contains(v_next)) {
        break;
      }
      v_curr = v_next;
      visited_verts.add(v_curr);
      list.append(v_curr);
      r_visited.add(e_next);
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
    if (math::distance_squared(float3(chain_data.verts[i]->co),
                               float3(chain_data.verts[i + 1]->co)) >
        math::square(STACKED_THRESHOLD))
    {
      all_stacked = false;
      break;
    }
  }
  if (all_stacked) {
    return std::nullopt;
  }
  /* Close the ring, ensuring the closing vertex is *not* a junction. */
  BMVert *v_first = chain_data.verts.first();
  BMVert *v_last = chain_data.verts.last();
  BMEdge *closing_edge = BM_edge_exists(v_first, v_last);
  if (closing_edge && BM_elem_flag_test(closing_edge, BM_ELEM_TAG) &&
      vert_next_walk_edge(v_first, r_visited) == closing_edge &&
      vert_next_walk_edge(v_last, r_visited) == closing_edge)
  {
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
 * Compute cumulative distances along the chain.
 */
static SpaceMeasurements measure_chain(const SpaceChainData &chain)
{
  SpaceMeasurements measure;
  const int num_verts = chain.verts.size();

  measure.positions.reinitialize(num_verts);
  for (const int i : IndexRange(num_verts)) {
    measure.positions[i] = float3(chain.verts[i]->co);
  }
  measure.knot_distances.reinitialize(num_verts + (chain.is_closed ? 1 : 0));
  measure.knot_distances[0] = 0.0f;
  length_parameterize::accumulate_lengths<float3>(
      measure.positions, chain.is_closed, measure.knot_distances.as_mutable_span().drop_front(1));

  measure.total_length = measure.knot_distances.last();
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
    Array<float> lower_diag(num_verts);
    Array<float> diag(num_verts);
    Array<float> upper_diag(num_verts);
    Array<float> rhs(num_verts);
    for (const int i : IndexRange(num_verts)) {
      const int v_prev = math::mod_periodic(i - 1, num_verts);
      const int v_next = math::mod_periodic(i + 1, num_verts);
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
    const int v_next = is_closed ? math::mod_periodic(i + 1, num_verts) : i + 1;

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
  const int segment_index = binary_search::last_if(
      knot_distances, [&](const float value) { return value <= target_distance; });
  return std::clamp(segment_index, 0, int(knot_distances.size()) - 2);
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

void bmo_space_edge_loops_evenly_exec(BMesh *bm, BMOperator *op)
{
  const float influence = BMO_slot_float_get(op->slots_in, "factor");
  const SpaceInterpolationMethod interpolation = static_cast<SpaceInterpolationMethod>(
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
    const int num_verts = chain.verts.size();
    SpaceMeasurements measure = measure_chain(chain);

    Array<int> sample_indices(num_verts);
    Array<float> sample_factors(num_verts);
    length_parameterize::sample_uniform(measure.knot_distances.as_span().drop_front(1),
                                        !chain.is_closed,
                                        sample_indices,
                                        sample_factors);

    Array<float3> new_positions(num_verts);

    if (interpolation == SPACE_INTERP_LINEAR) {
      length_parameterize::interpolate<float3>(
          measure.positions, sample_indices, sample_factors, new_positions);
    }
    else {
      Array<float> coords_x(num_verts);
      Array<float> coords_y(num_verts);
      Array<float> coords_z(num_verts);
      for (const int i : IndexRange(num_verts)) {
        coords_x[i] = measure.positions[i].x;
        coords_y[i] = measure.positions[i].y;
        coords_z[i] = measure.positions[i].z;
      }

      Vector<SplineCoeffs> coeffs_x;
      Vector<SplineCoeffs> coeffs_y;
      Vector<SplineCoeffs> coeffs_z;
      calculate_splines_axis(
          measure.knot_distances, coords_x, chain.is_closed, measure.total_length, coeffs_x);
      calculate_splines_axis(
          measure.knot_distances, coords_y, chain.is_closed, measure.total_length, coeffs_y);
      calculate_splines_axis(
          measure.knot_distances, coords_z, chain.is_closed, measure.total_length, coeffs_z);

      for (const int i : IndexRange(num_verts)) {
        const int seg = sample_indices[i];
        const float target_dist = math::interpolate(
            measure.knot_distances[seg], measure.knot_distances[seg + 1], sample_factors[i]);

        new_positions[i] = evaluate_cubic(
            measure.knot_distances, coeffs_x, coeffs_y, coeffs_z, target_dist);
      }
    }

    for (const int i : IndexRange(num_verts)) {
      /* The first and last vertices of an open chain are anchor points so they are skipped. */
      if (!chain.is_closed && (i == 0 || i == num_verts - 1)) {
        continue;
      }

      float3 new_pos = new_positions[i];

      if (lock_x) {
        new_pos.x = measure.positions[i].x;
      }
      if (lock_y) {
        new_pos.y = measure.positions[i].y;
      }
      if (lock_z) {
        new_pos.z = measure.positions[i].z;
      }

      float3 final_pos = math::interpolate(measure.positions[i], new_pos, influence);
      copy_v3_v3(chain.verts[i]->co, final_pos);
    }
  }
}

}  // namespace blender
