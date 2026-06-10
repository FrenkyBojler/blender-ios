/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Redistributes interior vertices of selected edge loops along linear
 * or Hermite spline interpolation path between loop endpoints.
 */

#include "BLI_array.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

struct EdgeFlowLoop {
  Vector<BMVert *> verts;
  bool is_cyclic;
};

static void edge_flow_collect_loops(BMesh *bm, Vector<EdgeFlowLoop> &r_loops)
{
  BMIter eiter;
  BMEdge *e;

  BMWalker walker;
  BMW_init(&walker,
           bm,
           BMW_EDGELOOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_FLAG_NOP,
           BMW_NIL_LAY,
           BMWDelimitFlag(0));

  BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
    if (!BM_elem_flag_test(e, BM_ELEM_TAG)) {
      continue;
    }

    /* Collect tagged edges from this topological loop in walker order. */
    Vector<BMEdge *> loop_edges;
    for (BMEdge *we = static_cast<BMEdge *>(BMW_begin(&walker, e)); we;
         we = static_cast<BMEdge *>(BMW_step(&walker)))
    {
      if (BM_elem_flag_test(we, BM_ELEM_TAG)) {
        loop_edges.append(we);
        BM_elem_flag_disable(we, BM_ELEM_TAG);
      }
    }

    if (loop_edges.size() < 2) {
      continue;
    }

    /* Find p1, vert of edges[0] not shared with edges[1] */
    BMVert *p1 = nullptr;
    if (loop_edges[0]->v1 != loop_edges[1]->v1 && loop_edges[0]->v1 != loop_edges[1]->v2) {
      p1 = loop_edges[0]->v1;
    }
    else {
      p1 = loop_edges[0]->v2;
    }

    /* Build ordered vert array. */
    EdgeFlowLoop loop;
    loop.verts.append(p1);
    BMVert *last = p1;
    for (BMEdge *le : loop_edges) {
      BMVert *next = BM_edge_other_vert(le, last);
      loop.verts.append(next);
      last = next;
    }

    loop.is_cyclic = (loop.verts.first() == loop.verts.last());
    r_loops.append(std::move(loop));
  }

  BMW_end(&walker);
}

static bool edge_flow_calc_spline_target(BMLoop *l, const float tension, float3 &r_target)
{
  if (l->f->len != 4 || l->radial_prev->f->len != 4) {
    return false;
  }

  BMLoop *ring1 = l->next->next;
  BMLoop *ring2 = l->radial_prev->prev->prev;

  BMVert *v2 = ring1->v;
  BMVert *v3 = ring2->radial_next->v;

  float3 p1;
  float3 p2 = v2->co;
  float3 p3 = v3->co;
  float3 p4;

  if (!BM_edge_is_boundary(ring1->e)) {
    BMLoop *l_outer = ring1->radial_next->next;
    BMVert *v1 = BM_edge_other_vert(l_outer->e, v2);

    if (v1 == nullptr) {
      return false;
    }

    p1 = v1->co;
  }
  else {
    /* Create a phantom control point and reflect p3 through p2. */
    p1 = p2 - (p3 - p2);
  }

  if (!BM_edge_is_boundary(ring2->e)) {
    BMLoop *l_outer = ring2->radial_prev->prev;
    BMVert *v4 = BM_edge_other_vert(l_outer->e, v3);

    if (v4 == nullptr) {
      return false;
    }

    p4 = v4->co;
  }
  else {
    /* Set v3 to the far vert first. Create a phantom control point and reflect p3 through p2. */
    v3 = BM_edge_other_vert(ring2->e, v3);
    p3 = v3->co;
    p4 = p3 - (p2 - p3);
  }

  if (p1 == p2 || p3 == p4) {
    return false;
  }

  /* Normalize arm lengths before spline interpolation to control the shape of the curve. */
  const float d = math::distance(p2, p3) * 0.5f;
  p1 = p2 + d * math::normalize(p1 - p2);
  p4 = p3 + d * math::normalize(p4 - p3);

  r_target = math::hermite_spline_interp(p1, p2, p3, p4, 0.5f, -tension, 0.0f);
  return true;
}

void bmo_edge_flow_exec(BMesh *bm, BMOperator *op)
{
  const int mode = BMO_slot_int_get(op->slots_in, "mode");
  const float mix = BMO_slot_float_get(op->slots_in, "mix");
  const bool space_evenly = BMO_slot_bool_get(op->slots_in, "space_evenly");
  const int tension_int = BMO_slot_int_get(op->slots_in, "tension");
  const int iterations  = BMO_slot_int_get(op->slots_in, "iterations");
  const float tension   = float(tension_int) / 100.0f;

  /* Tag edges passed in via the slot, then collect ordered loops */
  BMO_slot_buffer_hflag_enable(bm, op->slots_in, "edges", BM_EDGE, BM_ELEM_TAG, false);

  Vector<EdgeFlowLoop> loops;
  edge_flow_collect_loops(bm, loops);

  for (EdgeFlowLoop &loop : loops) {
    if (loop.is_cyclic) {
      continue;
    }

    /* No interior verts. */
    if (int(loop.verts.size()) <= 2) {
      continue;
    }

    Array<float3> orig_cos(loop.verts.size());
    for (const int i : loop.verts.index_range()) {
      copy_v3_v3(orig_cos[i], loop.verts[i]->co);
    }

    BMVert *p1 = loop.verts.first();
    BMVert *p2 = loop.verts.last();

    if (mode == EDGE_FLOW_LINEAR) {
      if (space_evenly) {
        const int count = int(loop.verts.size()) - 1;
        float3 dir;
        sub_v3_v3v3(dir, p2->co, p1->co);

        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          float3 co;
          madd_v3_v3v3fl(co, p1->co, dir, float(i) / float(count));

          float3 blended;
          interp_v3_v3v3(blended, orig_cos[i], co, mix);
          copy_v3_v3(loop.verts[i]->co, blended);
        }
      }
      else {
        /* space_evenly off flag */
        float3 dir;
        float3 dir_norm;
        sub_v3_v3v3(dir, p2->co, p1->co);
        normalize_v3_v3(dir_norm, dir);

        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          float3 co;
          sub_v3_v3v3(co, orig_cos[i], p1->co);
          float dir_scalar = dot_v3v3(co, dir_norm);

          float3 new_co;
          madd_v3_v3v3fl(new_co, p1->co, dir_norm, dir_scalar);

          float3 blended;
          interp_v3_v3v3(blended, orig_cos[i], new_co, mix);
          copy_v3_v3(loop.verts[i]->co, blended);
        }
      }
    }
    else if (mode == EDGE_FLOW_FLOW) {
      for (int iter = 0; iter < iterations; iter++) {
        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          BMVert *v = loop.verts[i];
          BMEdge *edges[2] = {BM_edge_exists(v, loop.verts[i - 1]), BM_edge_exists(v, loop.verts[i + 1])};

          float3 target_sum(0.0f);
          int target_count = 0;

          for (BMEdge *e : edges) {
            if (e == nullptr || e->l == nullptr || BM_edge_is_boundary(e)) {
              continue;
            }

            BMIter l_iter;
            BMLoop *l;
            BM_ITER_ELEM (l, &l_iter, e, BM_LOOPS_OF_EDGE) {
              if (BM_edge_other_vert(e, l->v) != v) {
                continue;
              }
              float3 target;

              if (edge_flow_calc_spline_target(l, tension, target)) {
                target_sum += target;
                target_count++;
              }
            }
          }
        }
      }
    }

    /* Reinterpolate UVs/customData for every face loop touching a moved vert. */
    // for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
    //   BMIter l_iter;
    //   BMLoop *l;
    //   BM_ITER_ELEM (l, &l_iter, loop.verts[i], BM_LOOPS_OF_VERT) {
    //     BM_loop_interp_from_face(bm, l, l->f, false, true);
    //   }
    // }
  }
}

}  // namespace blender
