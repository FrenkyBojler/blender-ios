/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Relaxes vertices along edge loops so they are smoother.
 */

#include "BLI_math_vector.hh"

#include "BLI_array_utils.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_math_solvers.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include <array>

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

/**
 * A chain of vertices collected from a walk along connected edges.
 */
struct RelaxChainData {
  /** Ordered vertices along the chain path. */
  Vector<BMVert *> verts;
  /** True if the path forms a closed chain. */
  bool is_closed = false;
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

/**
 * Defines which vertices stay still to define the shape
 * and which vertices are actively being relaxed.
 */
struct RelaxPhase {
  /** Indices of vertices used as static anchors for the spline. */
  Vector<int> knot_indices;
  /** Indices of vertices whose positions are being updated. */
  Vector<int> point_indices;
};

/** Epsilon to prevent zero division. */
constexpr float RELAX_EPSILON = 1e-8f;

/**
 * Compute cubic spline coefficients for one coordinate axis.
 * Uses `BLI_tridiagonal_solve` for open chains and
 * `BLI_tridiagonal_solve_cyclic` for closed loops.
 */
static void calculate_splines_axis(Span<float> distances,
                                   Span<float> coords,
                                   const bool is_closed,
                                   Vector<SplineCoeffs> &r_coeffs)
{
  const int verts_num = coords.size();
  if (verts_num < 2) {
    return;
  }
  const int num_segments = is_closed ? verts_num : verts_num - 1;
  Array<float> segment_length(num_segments);

  for (const int i : IndexRange(num_segments)) {
    segment_length[i] = distances[i + 1] - distances[i];
    if (!(segment_length[i] > 0.0f)) {
      segment_length[i] = RELAX_EPSILON;
    }
  }

  /* Stores second derivative coefficients. For a natural cubic spline, the boundary
   * condition defines the first and last points as zero. */
  Array<float> c_vals(verts_num, 0.0f);

  /* The Thomas algorithm used in `BLI_tridiagonal_solve` can't properly solve
   * a cyclic tridiagonal system so in this case, we use the Sherman-Morrison formula
   * via `BLI_tridiagonal_solve_cyclic`. */
  if (is_closed) {
    Array<float> lower_diag(verts_num);
    Array<float> diag(verts_num);
    Array<float> upper_diag(verts_num);
    Array<float> rhs(verts_num);
    for (const int i : IndexRange(verts_num)) {
      const int i_prev = math::mod_periodic(i - 1, verts_num);
      const int i_next = math::mod_periodic(i + 1, verts_num);
      lower_diag[i] = segment_length[i_prev];
      diag[i] = 2.0f * (segment_length[i_prev] + segment_length[i]);
      upper_diag[i] = segment_length[i];
      rhs[i] = 3.0f * (((coords[i_next] - coords[i]) / segment_length[i]) -
                       ((coords[i] - coords[i_prev]) / segment_length[i_prev]));
    }
    BLI_tridiagonal_solve_cyclic(
        lower_diag.data(), diag.data(), upper_diag.data(), rhs.data(), c_vals.data(), verts_num);
  }
  else {
    /* For a natural cubic spline the curvature at the first and last point
     * is 0, so for n given points, we only have n-2 unknown interior points. */
    const int interior = verts_num - 2;
    Array<float> lower_diag(interior);
    Array<float> diag(interior);
    Array<float> upper_diag(interior);
    Array<float> rhs(interior);

    for (const int i_curr : IndexRange(interior)) {
      const int i_next = i_curr + 1;
      lower_diag[i_curr] = segment_length[i_curr];
      diag[i_curr] = 2.0f * (segment_length[i_curr] + segment_length[i_next]);
      upper_diag[i_curr] = segment_length[i_next];
      rhs[i_curr] = 3.0f * (((coords[i_next + 1] - coords[i_next]) / segment_length[i_next]) -
                            ((coords[i_next] - coords[i_curr]) / segment_length[i_curr]));
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
    const int i_next = is_closed ? math::mod_periodic(i + 1, verts_num) : i + 1;

    const float coeff_a = coords[i];
    const float coeff_b = ((coords[i_next] - coords[i]) / segment_length[i]) -
                          (segment_length[i] * (c_vals[i_next] + 2.0f * c_vals[i])) / 3.0f;
    const float coeff_c = c_vals[i];
    const float coeff_d = (c_vals[i_next] - c_vals[i]) / (3.0f * segment_length[i]);
    r_coeffs.append({coeff_a, coeff_b, coeff_c, coeff_d, distances[i]});
  }
}

static void build_relax_phases(int num_verts, bool is_closed, Vector<RelaxPhase> &r_phases)
{
  if (!is_closed) {
    /* There are two relax phases, in the first phase, odd vertices are relaxed
     * and even ones are not(they're knots in this case), in second phase, even vertices
     * are relaxed and odd ones are not. The first and last vertices are not included to
     * be moved. */
    for (const int phase_index : IndexRange(2)) {
      RelaxPhase phase;
      for (const int i : IndexRange(num_verts)) {
        if (i % 2 == phase_index) {
          phase.knot_indices.append(i);
        }
        else if (i > 0 && i < num_verts - 1) {
          phase.point_indices.append(i);
        }
      }
      r_phases.append(std::move(phase));
    }
    return;
  }

  Vector<int> vert_indices(num_verts);
  array_utils::fill_index_range(vert_indices.as_mutable_span());

  for (const int j : IndexRange(2)) {
    const bool extend = num_verts % 2 == 1 ? j == 1 : j == 0;
    const int knot_start = !extend && j == 1 ? 1 : 0;
    const int point_start = !extend && j == 1 ? 2 : 1;

    if (extend) {
      const int last_vert = vert_indices.last();
      const int first_vert = vert_indices.first();
      vert_indices.insert(0, last_vert);
      vert_indices.append(first_vert);
    }

    RelaxPhase phase;
    for (int i = knot_start; i < vert_indices.size(); i += 2) {
      phase.knot_indices.append(vert_indices[i]);
    }
    for (int i = point_start; i < vert_indices.size(); i += 2) {
      const int val = vert_indices[i];
      if (phase.point_indices.is_empty() || val != phase.point_indices.first()) {
        phase.point_indices.append(val);
      }
    }
    if (phase.knot_indices.first() != phase.knot_indices.last()) {
      phase.knot_indices.append(phase.knot_indices.first());
    }

    if (!phase.point_indices.is_empty()) {
      r_phases.append(std::move(phase));
    }
  }
}

static RelaxChainData walk_edges(BMEdge *start_edge, Set<BMEdge *> &r_visited)
{
  RelaxChainData chain_data;
  Set<BMVert *> visited_verts;

  chain_data.verts.append(start_edge->v1);
  chain_data.verts.append(start_edge->v2);
  visited_verts.add(start_edge->v1);
  visited_verts.add(start_edge->v2);
  r_visited.add(start_edge);

  auto walk_fn = [&](BMVert *v_curr, Vector<BMVert *> &list) {
    while (true) {
      BMEdge *e_next = nullptr;
      BMIter eiter;
      BMEdge *e_candidate;
      BM_ITER_ELEM (e_candidate, &eiter, v_curr, BM_EDGES_OF_VERT) {
        if (!r_visited.contains(e_candidate) && BM_elem_flag_test(e_candidate, BM_ELEM_TAG)) {
          e_next = e_candidate;
          break;
        }
      }
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

  walk_fn(start_edge->v2, chain_data.verts);
  Vector<BMVert *> pre_chain;
  walk_fn(start_edge->v1, pre_chain);
  if (!pre_chain.is_empty()) {
    std::reverse(pre_chain.begin(), pre_chain.end());
    pre_chain.extend(chain_data.verts);
    chain_data.verts = std::move(pre_chain);
  }

  if (chain_data.verts.size() > 2) {
    BMEdge *closing_edge = BM_edge_exists(chain_data.verts.first(), chain_data.verts.last());
    chain_data.is_closed = closing_edge && BM_elem_flag_test(closing_edge, BM_ELEM_TAG);
  }
  return chain_data;
}

static void get_relax_input_chains(BMesh *bm, Vector<RelaxChainData> &r_chains)
{
  Set<BMEdge *> visited;
  BMIter iter;
  BMEdge *edge;
  BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
    if (!BM_elem_flag_test(edge, BM_ELEM_TAG) || visited.contains(edge)) {
      continue;
    }
    RelaxChainData chain = walk_edges(edge, visited);
    if (chain.verts.size() >= 3) {
      r_chains.append(std::move(chain));
    }
  }
}

static void calculate_relax_t(Span<BMVert *> verts,
                              const RelaxPhase &phase,
                              const bool regular,
                              Vector<float> &r_t_knots,
                              Vector<float> &r_t_points)
{
  const int n_knots = phase.knot_indices.size();
  const int n_points = phase.point_indices.size();
  const int total = n_knots + n_points;

  Array<float3> positions(total);
  for (const int i : IndexRange(total)) {
    int vert_index;
    if (i % 2 == 0) {
      vert_index = phase.knot_indices[i / 2];
    }
    else if (i == total - 1) {
      vert_index = phase.knot_indices.last();
    }
    else {
      vert_index = phase.point_indices[i / 2];
    }
    positions[i] = verts[vert_index]->co;
  }

  Array<float> cumulative(total);
  cumulative[0] = 0.0f;
  length_parameterize::accumulate_lengths<float3>(
      positions, false, cumulative.as_mutable_span().drop_front(1));

  for (const int i : IndexRange(total)) {
    if (i % 2 == 0 || i == total - 1) {
      r_t_knots.append(cumulative[i]);
    }
    else {
      r_t_points.append(cumulative[i]);
    }
  }

  /* Place a point halfway between two knots if regular is enabled. */
  if (regular) {
    r_t_points.clear();
    for (const int p : IndexRange(n_points)) {
      r_t_points.append((r_t_knots[p] + r_t_knots[p + 1]) / 2.0f);
    }
  }
}

static void calculate_relax_splines(Span<BMVert *> verts,
                                    Span<int> knot_indices,
                                    Span<float> t_params,
                                    bool is_closed,
                                    std::array<Vector<SplineCoeffs>, 3> &r_coeffs)
{
  const int num_knots = knot_indices.size();
  const int coords_size = is_closed ? num_knots - 1 : num_knots;
  Array<float> coords_x(coords_size);
  Array<float> coords_y(coords_size);
  Array<float> coords_z(coords_size);

  for (const int i : IndexRange(coords_size)) {
    const float *co = verts[knot_indices[i]]->co;
    coords_x[i] = co[0];
    coords_y[i] = co[1];
    coords_z[i] = co[2];
  }

  calculate_splines_axis(t_params, coords_x, is_closed, r_coeffs[0]);
  calculate_splines_axis(t_params, coords_y, is_closed, r_coeffs[1]);
  calculate_splines_axis(t_params, coords_z, is_closed, r_coeffs[2]);
}

static void execute_relax_phase(
    Span<BMVert *> verts, const RelaxPhase &phase, bool is_closed, int interpolation, bool regular)
{
  if (phase.point_indices.is_empty()) {
    return;
  }

  Vector<float> t_knots, t_points;
  calculate_relax_t(verts, phase, regular, t_knots, t_points);

  const Span<float> accumulated_lengths = Span<float>(t_knots).drop_front(1);

  const int num_points = phase.point_indices.size();
  Array<int> segment_indices(num_points);
  Array<float> factors(num_points);
  length_parameterize::sample_at_lengths(accumulated_lengths, t_points, segment_indices, factors);

  Array<float3> sampled_positions(num_points);

  if (interpolation == RELAX_EDGE_LOOPS_INTERP_LINEAR) {
    Array<float3> knot_positions(phase.knot_indices.size());
    for (const int i : phase.knot_indices.index_range()) {
      knot_positions[i] = verts[phase.knot_indices[i]]->co;
    }

    length_parameterize::interpolate<float3>(
        knot_positions, segment_indices, factors, sampled_positions);
  }
  else {
    std::array<Vector<SplineCoeffs>, 3> axis_coeffs;
    calculate_relax_splines(verts, phase.knot_indices, t_knots, is_closed, axis_coeffs);

    for (const int i : IndexRange(num_points)) {
      const int seg = segment_indices[i];
      const float dt = t_points[i] - axis_coeffs[0][seg].x;

      const SplineCoeffs &cx = axis_coeffs[0][seg];
      const SplineCoeffs &cy = axis_coeffs[1][seg];
      const SplineCoeffs &cz = axis_coeffs[2][seg];

      sampled_positions[i] = float3(cx.a + dt * (cx.b + dt * (cx.c + dt * cx.d)),
                                    cy.a + dt * (cy.b + dt * (cy.c + dt * cy.d)),
                                    cz.a + dt * (cz.b + dt * (cz.c + dt * cz.d)));
    }
  }

  for (const int i : IndexRange(num_points)) {
    const int v_index = phase.point_indices[i];
    const float3 current_pos(verts[v_index]->co);
    const float3 final_pos = (current_pos + sampled_positions[i]) / 2.0f;
    verts[v_index]->co[0] = final_pos.x;
    verts[v_index]->co[1] = final_pos.y;
    verts[v_index]->co[2] = final_pos.z;
  }
}

void bmo_relax_edge_loops_exec(BMesh *bm, BMOperator *op)
{
  const int iterations = BMO_slot_int_get(op->slots_in, "iterations");
  const int interpolation = BMO_slot_int_get(op->slots_in, "interpolation");
  const bool regular = BMO_slot_bool_get(op->slots_in, "regular");

  BM_mesh_elem_hflag_disable_all(bm, BM_EDGE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(bm, op->slots_in, "geom", BM_EDGE, BM_ELEM_TAG, false);

  Vector<RelaxChainData> chains;
  get_relax_input_chains(bm, chains);

  for (const int it : IndexRange(iterations)) {
    UNUSED_VARS(it);

    for (RelaxChainData &chain : chains) {
      Vector<RelaxPhase> phases;
      build_relax_phases(chain.verts.size(), chain.is_closed, phases);
      for (const RelaxPhase &phase : phases) {
        execute_relax_phase(chain.verts, phase, chain.is_closed, interpolation, regular);
      }
    }
  }
}

}  // namespace blender
