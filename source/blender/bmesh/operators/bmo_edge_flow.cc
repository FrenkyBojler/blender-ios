/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Redistributes interior vertices of selected edge loops along a linear
 * or (future) Hermite spline path between the loop endpoints.
 */

#include "BLI_array.hh"
#include "BLI_math_vector.h"
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
  /* Edges are already tagged by the caller via BMO_slot_buffer_hflag_enable. */
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

void bmo_edge_flow_exec(BMesh *bm, BMOperator *op)
{
  const int mode = BMO_slot_int_get(op->slots_in, "mode");
  const float mix = BMO_slot_float_get(op->slots_in, "mix");
  const bool space_evenly = BMO_slot_bool_get(op->slots_in, "space_evenly");

  /* Tag edges passed in via the slot, then collect ordered loops */
  BMO_slot_buffer_hflag_enable(bm, op->slots_in, "edges", BM_EDGE, BM_ELEM_TAG, false);

  Vector<EdgeFlowLoop> loops;
  edge_flow_collect_loops(bm, loops);

  for (EdgeFlowLoop &loop : loops) {
    if (loop.is_cyclic) {
      continue;
    }
    if ((int)loop.verts.size() <= 2) {
      continue; /* no interior verts */
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
        float dir[3];
        sub_v3_v3v3(dir, p2->co, p1->co);

        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          float co[3];
          madd_v3_v3v3fl(co, p1->co, dir, float(i) / float(count));

          float blended[3];
          interp_v3_v3v3(blended, orig_cos[i], co, mix);
          copy_v3_v3(loop.verts[i]->co, blended);
        }
      }
      else {
        float dir[3], dir_norm[3];
        sub_v3_v3v3(dir, p2->co, p1->co);
        normalize_v3_v3(dir_norm, dir);

        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          float co[3];
          sub_v3_v3v3(co, orig_cos[i], p1->co);
          float scalar = dot_v3v3(co, dir_norm);

          float new_co[3];
          madd_v3_v3v3fl(new_co, p1->co, dir_norm, scalar);

          float blended[3];
          interp_v3_v3v3(blended, orig_cos[i], new_co, mix);
          copy_v3_v3(loop.verts[i]->co, blended);
        }
      }
    }
    /* EDGE_FLOW_FLOW */

    /* Reinterpolate normals for every face loop touching a moved vert. */
    for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
      BMIter liter;
      BMLoop *l;
      BM_ITER_ELEM (l, &liter, loop.verts[i], BM_LOOPS_OF_VERT) {
        BM_loop_interp_from_face(bm, l, l->f, false, true);
      }
    }
  }
}

}  // namespace blender
