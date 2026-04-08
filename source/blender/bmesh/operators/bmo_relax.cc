/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Relaxes vertices along edge loops so they are smoother.
 */

#include "BLI_array_utils.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

/**
 * A chain of vertices collected from a walk along connected edges.
 */
struct RelaxChainData {
  /** Ordered vertices along the chain path. */
  Vector<BMVert *> verts;
  /** True if the path forms a closed loop. */
  bool is_closed;
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

/**
 * Temporarly stores the calculated new position for a vertex.
 */
struct PendingMove {
  /** The vertex to be moved. */
  BMVert *v;
  /** The target position for the vertex. */
  float3 new_pos;
};

enum { CUBIC = 0, LINEAR = 1 };

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
  Array<float> h(n - 1);
  /* Forward elimination variables. */
  Array<float> l(n);
  Array<float> u(n);
  Array<float> z(n);
  /* The final polynomial coefficients. */
  Array<float> c(n);
  Array<float> b(n);
  Array<float> d(n);

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

/** Return the index of the spline segment that contains target_distance. */
static int find_spline_segment(Span<float> knot_distances, float target_distance)
{
  for (const int k : IndexRange(knot_distances.size() - 1)) {
    if (target_distance >= knot_distances[k] && target_distance <= knot_distances[k + 1]) {
      return k;
    }
  }
  return knot_distances.size() - 2;
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
      int old_last = vert_indices.last();
      int old_first = vert_indices.first();
      vert_indices.insert(0, old_last);
      vert_indices.append(old_first);
    }

    RelaxPhase phase;
    for (int i = knot_start; i < vert_indices.size(); i += 2) {
      phase.knot_indices.append(vert_indices[i]);
    }
    for (int i = point_start; i < vert_indices.size(); i += 2) {
      int val = vert_indices[i];
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

  walk_fn(start_edge->v2, chain_data.verts);
  Vector<BMVert *> pre_chain;
  walk_fn(start_edge->v1, pre_chain);
  if (!pre_chain.is_empty()) {
    std::reverse(pre_chain.begin(), pre_chain.end());
    pre_chain.extend(chain_data.verts);
    chain_data.verts = std::move(pre_chain);
  }

  BMEdge *closing_edge = BM_edge_exists(chain_data.verts.first(), chain_data.verts.last());
  chain_data.is_closed = closing_edge && BM_elem_flag_test(closing_edge, BM_ELEM_TAG);
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

  float cumulative_length = 0.0f;
  float3 prev_loc = float3(verts[phase.knot_indices[0]]->co);

  for (const int i : IndexRange(n_knots + n_points)) {
    int vert_index;
    bool is_knot = i % 2 == 0;

    if (is_knot) {
      vert_index = phase.knot_indices[i / 2];
    }
    else {
      vert_index = phase.point_indices[i / 2];
    }

    const float3 curr_loc(verts[vert_index]->co);
    cumulative_length += math::distance(curr_loc, prev_loc);

    if (is_knot) {
      r_t_knots.append(cumulative_length);
    }
    else {
      r_t_points.append(cumulative_length);
    }
    prev_loc = curr_loc;
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
                                    int interpolation,
                                    Vector<SplineCoeffs> (&r_coeffs)[3])
{
  const int n = knot_indices.size();
  Array<float> coords_x(n);
  Array<float> coords_y(n);
  Array<float> coords_z(n);

  for (const int i : IndexRange(n)) {
    const float *co = verts[knot_indices[i]]->co;
    coords_x[i] = co[0];
    coords_y[i] = co[1];
    coords_z[i] = co[2];
  }

  const bool is_circular = (knot_indices.first() == knot_indices.last());

  if (interpolation == CUBIC) {
    if (is_circular) {
      const int padding = 4;
      const int period = n - 1;

      Vector<float> t_ext, x_ext, y_ext, z_ext;

      for (int i = -padding; i < n + padding; i++) {
        const int wrapped_index = mod_i(i, period);

        const float lap_offset = floorf(float(i) / period) * t_params[period];
        t_ext.append(t_params[wrapped_index] + lap_offset);

        x_ext.append(coords_x[wrapped_index]);
        y_ext.append(coords_y[wrapped_index]);
        z_ext.append(coords_z[wrapped_index]);
      }

      Vector<SplineCoeffs> cx_ext, cy_ext, cz_ext;
      solve_thomas_algorithm(t_ext, x_ext, cx_ext);
      solve_thomas_algorithm(t_ext, y_ext, cy_ext);
      solve_thomas_algorithm(t_ext, z_ext, cz_ext);

      for (int i : IndexRange(padding, period)) {
        r_coeffs[0].append(cx_ext[i]);
        r_coeffs[1].append(cy_ext[i]);
        r_coeffs[2].append(cz_ext[i]);
      }
    }
    else {
      solve_thomas_algorithm(t_params, coords_x, r_coeffs[0]);
      solve_thomas_algorithm(t_params, coords_y, r_coeffs[1]);
      solve_thomas_algorithm(t_params, coords_z, r_coeffs[2]);
    }
  }
}

static void execute_relax_phase(Span<BMVert *> verts,
                                const RelaxPhase &phase,
                                int interpolation,
                                bool regular,
                                Vector<PendingMove> &r_moves)
{
  Vector<float> t_knots, t_points;
  calculate_relax_t(verts, phase, regular, t_knots, t_points);

  Vector<SplineCoeffs> axis_coeffs[3];

  if (interpolation == CUBIC) {
    calculate_relax_splines(verts, phase.knot_indices, t_knots, interpolation, axis_coeffs);
  }

  for (const int i : phase.point_indices.index_range()) {
    float target_dist = t_points[i];
    int seg = find_spline_segment(t_knots, target_dist);
    float3 spline_pos;

    if (interpolation == LINEAR) {
      float factor = (target_dist - t_knots[seg]) / (t_knots[seg + 1] - t_knots[seg]);
      spline_pos = math::interpolate(float3(verts[phase.knot_indices[seg]]->co),
                                     float3(verts[phase.knot_indices[seg + 1]]->co),
                                     factor);
    }
    else {
      const float dt = target_dist - axis_coeffs[0][seg].x;
      const float dt2 = dt * dt;
      const float dt3 = dt2 * dt;

      const SplineCoeffs &cx = axis_coeffs[0][seg];
      const SplineCoeffs &cy = axis_coeffs[1][seg];
      const SplineCoeffs &cz = axis_coeffs[2][seg];

      spline_pos = float3(cx.a + cx.b * dt + cx.c * dt2 + cx.d * dt3,
                          cy.a + cy.b * dt + cy.c * dt2 + cy.d * dt3,
                          cz.a + cz.b * dt + cz.c * dt2 + cz.d * dt3);
    }

    int v_index = phase.point_indices[i];
    float3 current_pos(verts[v_index]->co);
    float3 final_pos = (current_pos + spline_pos) / 2.0f;

    r_moves.append({verts[v_index], final_pos});
  }
}

void bmo_relax_exec(BMesh *bm, BMOperator *op)
{
  const int iterations = BMO_slot_int_get(op->slots_in, "iterations");
  const int interpolation = BMO_slot_int_get(op->slots_in, "interpolation");
  const bool regular = BMO_slot_bool_get(op->slots_in, "regular");

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BMO_slot_buffer_hflag_enable(
      bm, op->slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  Vector<RelaxChainData> chains;
  get_relax_input_chains(bm, chains);

  for (const int it : IndexRange(iterations)) {
    UNUSED_VARS(it);
    Vector<PendingMove> pending_moves;

    for (RelaxChainData &chain : chains) {
      Vector<RelaxPhase> phases;
      build_relax_phases(chain.verts.size(), chain.is_closed, phases);
      for (const RelaxPhase &phase : phases) {
        execute_relax_phase(chain.verts, phase, interpolation, regular, pending_moves);
      }
    }

    for (const PendingMove &move : pending_moves) {
      move.v->co[0] = move.new_pos.x;
      move.v->co[1] = move.new_pos.y;
      move.v->co[2] = move.new_pos.z;
    }
  }
}

}  // namespace blender
