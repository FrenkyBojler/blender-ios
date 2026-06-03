/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 *
 * Apply linear or Hermite spline interpolation to selected edge loops to modify curvature.
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"

#include "BLI_math_geom.h"
#include "BLI_math_vector.h"

#include "WM_types.hh"

#include "ED_mesh.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include "mesh_intern.hh" /* own include */

namespace blender {

enum {
  MODE_LINEAR = 0,
  MODE_FLOW = 1,
};

struct EdgeFlowLoop {
  Vector<BMVert *> verts;
  bool is_cyclic;
};

static void edge_flow_collect_loops(BMesh *bm, Vector<EdgeFlowLoop> &r_loops)
{
  /* tag all selected edges for processing */
  BMIter eiter;
  BMEdge *e;
  BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
    BM_elem_flag_set(e, BM_ELEM_TAG, BM_elem_flag_test(e, BM_ELEM_SELECT));
  }

  BMWalker walker;
  BMW_init(&walker, bm, BMW_EDGELOOP, BMW_MASK_NOP, BMW_MASK_NOP, BMW_MASK_NOP, BMW_FLAG_NOP, BMW_NIL_LAY, BMWDelimitFlag(0));

  BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
    if (!BM_elem_flag_test(e, BM_ELEM_TAG)) {
      continue;
    }

    /* collect selected edges from this topological loop in walker order */
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
      continue; /* single edge */
    }

    /* find p1, vert of edges[0] not shared with edges[1] */
    BMVert *p1 = nullptr;
    if (loop_edges[0]->v1 != loop_edges[1]->v1 &&
        loop_edges[0]->v1 != loop_edges[1]->v2)
    {
      p1 = loop_edges[0]->v1;
    }
    else {
      p1 = loop_edges[0]->v2;
    }

    /* build ordered vert array */
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

static wmOperatorStatus edbm_edge_flow_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    bool changed = false;

    if (bm->totedgesel == 0) {
      continue;
    }

    Vector<EdgeFlowLoop> loops;
    edge_flow_collect_loops(bm, loops);

    if (loops.is_empty()) {
      continue;
    }

    const int mode = RNA_enum_get(op->ptr, "mode");
    const float mix = RNA_float_get(op->ptr, "mix");
    const bool space_evenly = RNA_boolean_get(op->ptr, "space_evenly");

    for (EdgeFlowLoop &loop : loops) {
      if (loop.is_cyclic) {
        continue;
      }

      if ((int)loop.verts.size() <= 2) {
        continue;
      }
      /* original vertex position snapshot */
      Array<float3> orig_cos(loop.verts.size());
      for (int i = 0; i < (int)loop.verts.size(); i++) {
        copy_v3_v3(orig_cos[i], loop.verts[i]->co);
      }

      BMVert *p1 = loop.verts.first();
      BMVert *p2 = loop.verts.last();

      if (mode == MODE_LINEAR) {
        for (int i = 1; i < (int)loop.verts.size() - 1; i++) {
          float t = float(i) / float(loop.verts.size() - 1);
          if (space_evenly) {
            t = sqrtf(t);
          }
          float3 co;
          interp_v3_v3v3(co, p1->co, p2->co, t);
          if (mix < 1.0f) {
            interp_v3_v3v3(co, orig_cos[i], co, mix);
          }
          copy_v3_v3(loop.verts[i]->co, co);
        }
      }
      else if (mode == MODE_FLOW) {

    }

    changed = true;


    if (changed) {
      EDBMUpdate_Params params{};
      params.calc_looptris = true;
      params.calc_normals = true;
      params.is_destructive = false;
      EDBM_update(id_cast<Mesh *>(obedit->data), &params);
    }
  }

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem mode_items[] = {
    {MODE_LINEAR, "LINEAR", 0, "Linear", "Straighten the loop between endpoints"},
    {MODE_FLOW, "FLOW", 0, "Flow", "Adjust loop to match surrounding geometry"},
    {0, nullptr, 0, nullptr, nullptr},
};

void MESH_OT_edge_flow(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Edge Flow";
  ot->idname = "MESH_OT_edge_flow";
  ot->description = "Set edge flow for the selected edges";

  /* API callbacks. */
  ot->exec = edbm_edge_flow_exec;
  ot->poll = EDBM_view3d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;

  prop = RNA_def_enum(ot->srna,
                      "mode",
                      mode_items,
                      MODE_LINEAR,
                      "Mode",
                      "");

  prop = RNA_def_float(ot->srna,
                      "mix",
                      1.0f,
                      0.0f,
                      1.0f,
                      "Mix",
                      "Mix between original and edge flow position.",
                      0.0f,
                      1.0f);

  prop = RNA_def_boolean(ot->srna,
                      "space_evenly",
                      false,
                      "Space Evenly",
                      "Space edges evenly along the loop.");
}

}  // namespace blender
