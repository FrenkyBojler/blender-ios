/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstddef>
#include <cstdlib>
#include <fmt/format.h>

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_heap_simple.h"
#include "BLI_linklist.h"
#include "BLI_linklist_stack.h"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_sort_utils.h"
#include "BLI_string.h"
#include "BLI_task.hh"

#include "BKE_attribute.h"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_deform.hh"
#include "BKE_editmesh.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "BLT_translation.hh"

#include "DNA_key_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_transform.hh"
#include "ED_uvedit.hh"
#include "ED_view3d.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "bmesh_tools.hh"
#include "transform.hh"
#include "transform_convert.hh"
#include "transform_mode.hh"
#include "transform_snap.hh"

namespace blender::ed::transform {

typedef struct TransEdgeMirrorData {
  BMEdge *edge;
  float mirror_ival;
} TransEdgeMirrorData;

/* -------------------------------------------------------------------- */
/** \name Transform Value
 * \{ */

static void transdata_elem_value(const TransInfo * /*t*/,
                                 const TransDataContainer * /*tc*/,
                                 TransData *td,
                                 const float value)
{
  if (td->val == nullptr)
    return;
  *td->val = td->ival + value * td->factor;
  CLAMP(*td->val, 0.0f, 1.0f);
}

static void apply_value_impl(TransInfo *t, const char *value_name)
{
  float value;
  char str[UI_MAX_DRAW_STR];

  value = t->values[0] + t->values_modal_offset[0];
  CLAMP_MAX(value, 1.0f);

  transform_snap_increment(t, &value);
  applyNumInput(&t->num, &value);
  t->values_final[0] = value;

  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];
    outputNumInput(&(t->num), c, t->scene->unit);
    if (value >= 0.0f) {
      SNPRINTF(str, "%s: +%s %s", value_name, c, t->proptext);
    }
    else {
      SNPRINTF(str, "%s: %s %s", value_name, c, t->proptext);
    }
  }
  else {
    if (value >= 0.0f) {
      SNPRINTF(str, "%s: +%.3f %s", value_name, value, t->proptext);
    }
    else {
      SNPRINTF(str, "%s: %.3f %s", value_name, value, t->proptext);
    }
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    threading::parallel_for(IndexRange(tc->data_len), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        TransData *td = &tc->data[i];
        if (td->flag & TD_SKIP) {
          continue;
        }
        transdata_elem_value(t, tc, td, value);
      }
    });
  }

  recalc_data(t);
  ED_area_status_text(t->area, str);
}

static void apply_value_impl_with_mirror(TransInfo *t, const char *value_name)
{
  float value;
  char str[UI_MAX_DRAW_STR];

  value = t->values[0] + t->values_modal_offset[0];
  CLAMP_MAX(value, 1.0f);

  transform_snap_increment(t, &value);
  applyNumInput(&t->num, &value);
  t->values_final[0] = value;

  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];
    outputNumInput(&(t->num), c, t->scene->unit);
    if (value >= 0.0f) {
      SNPRINTF(str, "%s: +%s %s", value_name, c, t->proptext);
    }
    else {
      SNPRINTF(str, "%s: %s %s", value_name, c, t->proptext);
    }
  }
  else {
    if (value >= 0.0f) {
      SNPRINTF(str, "%s: +%.3f %s", value_name, value, t->proptext);
    }
    else {
      SNPRINTF(str, "%s: %.3f %s", value_name, value, t->proptext);
    }
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    threading::parallel_for(IndexRange(tc->data_len), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        TransData *td = &tc->data[i];
        if (td->flag & TD_SKIP) {
          continue;
        }

        transdata_elem_value(t, tc, td, value);

        Mesh *me = static_cast<Mesh *>(tc->obedit->data);
        if (me && me->symmetry && td->extra) {
          BMEditMesh *em = BKE_editmesh_from_object(tc->obedit);
          TransEdgeMirrorData *med = static_cast<TransEdgeMirrorData *>(td->extra);
          if (med && med->edge) {
            for (int axis = 0; axis < 3; axis++) {
              const int axis_flag = (ME_SYMMETRY_X << axis);
              if (!(me->symmetry & axis_flag)) {
                continue;
              }
              const bool use_topology = ((me->editflag & ME_EDIT_MIRROR_TOPO) != 0);
              EDBM_verts_mirror_cache_begin(em, axis, false, true, false, use_topology);
              BMEdge *edge_mirror = EDBM_verts_mirror_get_edge(em, med->edge);
              if (edge_mirror) {
                int cd_edge_float_offset = (t->mode == TFM_BWEIGHT) ?
                                               CustomData_get_offset_named(&em->bm->edata,
                                                                           CD_PROP_FLOAT,
                                                                           "bevel_weight_edge") :
                                               CustomData_get_offset_named(
                                                   &em->bm->edata, CD_PROP_FLOAT, "crease_edge");
                if (cd_edge_float_offset != -1) {
                  float *mirror_val = static_cast<float *>(
                      BM_ELEM_CD_GET_VOID_P(edge_mirror, cd_edge_float_offset));
                  if (mirror_val) {
                    *mirror_val = med->mirror_ival + value * td->factor;
                    CLAMP(*mirror_val, 0.0f, 1.0f);
                  }
                }
              }
              EDBM_verts_mirror_cache_end(em);
            }
          }
        }
      }
    });
  }

  recalc_data(t);
  ED_area_status_text(t->area, str);
}

static void applyCrease(TransInfo *t)
{
  apply_value_impl_with_mirror(t, IFACE_("Crease"));
}

static void applyBevelWeight(TransInfo *t)
{
  apply_value_impl_with_mirror(t, IFACE_("Bevel Weight"));
}

static void init_mode_impl(TransInfo *t)
{
  initMouseInputMode(t, &t->mouse, INPUT_SPRING_DELTA);
  t->idx_max = 0;
  t->num.idx_max = 0;
  t->snap[0] = 0.1f;
  t->snap[1] = t->snap[0] * 0.1f;
  copy_v3_fl(t->num.val_inc, t->snap[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
}

static void initEgdeCrease(TransInfo *t, wmOperator * /*op*/)
{
  init_mode_impl(t);
  t->mode = TFM_EDGE_CREASE;
}

static void initVertCrease(TransInfo *t, wmOperator * /*op*/)
{
  init_mode_impl(t);
  t->mode = TFM_VERT_CREASE;
}

static void initBevelWeight(TransInfo *t, wmOperator * /*op*/)
{
  init_mode_impl(t);
  t->mode = TFM_BWEIGHT;
}

/** \} */

TransModeInfo TransMode_edgecrease = {
    /*flags*/ T_NO_CONSTRAINT | T_NO_PROJECT,
    /*init_fn*/ initEgdeCrease,
    /*transform_fn*/ applyCrease,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

TransModeInfo TransMode_vertcrease = {
    /*flags*/ T_NO_CONSTRAINT | T_NO_PROJECT,
    /*init_fn*/ initVertCrease,
    /*transform_fn*/ applyCrease,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

TransModeInfo TransMode_bevelweight = {
    /*flags*/ T_NO_CONSTRAINT | T_NO_PROJECT,
    /*init_fn*/ initBevelWeight,
    /*transform_fn*/ applyBevelWeight,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform
