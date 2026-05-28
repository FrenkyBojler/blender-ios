/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_action.hh"
#include "BKE_appdir.hh"
#include "BKE_blendfile.hh"
#include "BKE_context.hh"
#include "BKE_fcurve.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_types.hh"

#include "ED_anim_transformable.hh"
#include "ED_screen.hh"

#include "ANIM_action.hh"

#include "anim_intern.hh"

namespace blender::ed::animrig {

static void matrix_to_fcurves(const float4x4 &matrix,
                              Span<FCurve *> fcurves,
                              const int frame,
                              const int key_index)
{
  /* Using IndexRange(3) because the last row is always 0/0/0/1 which means we don't need to
   * store it. */
  for (const int y : IndexRange(3)) {
    for (const int x : IndexRange(4)) {
      FPoint &fpt = fcurves[y * 4 + x]->fpt[key_index];
      fpt.vec[0] = frame;
      fpt.vec[1] = matrix[x][y];
    }
  }
}

static float4x4 fcurves_to_matrix(const Span<const FCurve *> fcurves, const int key_index)
{
  float4x4 mat = float4x4::identity();
  for (const int y : IndexRange(3)) {
    for (const int x : IndexRange(4)) {
      FPoint &fpt = fcurves[y * 4 + x]->fpt[key_index];
      mat[x][y] = fpt.vec[1];
    }
  }
  return mat;
}

/**
 * \param range inclusive/exclusive
 */
static void copy_world_space(Main &bmain,
                             Scene &scene,
                             ViewLayer &view_layer,
                             ReportList &reports,
                             const Span<AnimTransformable> transformables,
                             const Bounds<int> range)
{
  namespace ar = blender::animrig;
  namespace bf = bke::blendfile;
  bf::PartialWriteContext copybuffer{bmain};
  bAction *dna_action = reinterpret_cast<bAction *>(
      copybuffer.id_create(ID_AC,
                           "world_space_copy",
                           nullptr,
                           {(bf::PartialWriteContext::IDAddOperations::SET_FAKE_USER |
                             bf::PartialWriteContext::IDAddOperations::SET_CLIPBOARD_MARK)}));

  ar::Action &action = dna_action->wrap();
  action.layer_keystrip_ensure();
  ar::StripKeyframeData &strip_data = action.layer(0)->strip(0)->data<ar::StripKeyframeData>(
      action);
  ar::Slot &slot = action.slot_add();
  ar::Channelbag &channelbag = strip_data.channelbag_for_slot_ensure(slot);

  /* We are storing the world space matrix in separate FCurves so the data can be stored in a
   * blend file. */
  Vector<ID *> ids;
  Map<const AnimTransformable *, Array<FCurve *>> world_space_data;
  for (const AnimTransformable &transformable : transformables) {
    Array<FCurve *> fcurves(12);

    ids.append(transformable.owner_id());
    for (const int i : fcurves.index_range()) {
      FCurve *fcurve = BKE_fcurve_create();
      fcurve->rna_path = BLI_strdupn(transformable.name().data(), transformable.name().size());
      fcurve->array_index = i;
      /* Using FPoint because we only need 2 floats per key, not the huge struct that
       * is BezTriple.  */
      fcurve->fpt = MEM_new_array_uninitialized<FPoint>(range.size(), "world_space_copy_points");
      fcurve->totvert = range.size();
      /* Could allocate space on the channelbag in big chunks instead of appending which is a
       * MEM_new every time. */
      channelbag.fcurve_append(*fcurve);
      fcurves[i] = fcurve;
    }

    if (!world_space_data.add(&transformable, fcurves)) {
      BLI_assert_unreachable();
    }
  }

  Depsgraph *depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  DEG_graph_build_from_ids(depsgraph, ids);

  for (int frame = range.min; frame < range.max; frame++) {
    const int key_index = frame - range.min;
    DEG_evaluate_on_framechange(depsgraph, frame);
    for (const AnimTransformable &transformable : transformables) {
      const float4x4 world_matrix = transformable.get_world_matrix();
      matrix_to_fcurves(world_matrix, world_space_data.lookup(&transformable), frame, key_index);
    }
  }

  DEG_graph_free(depsgraph);

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_base(), "world_space_buffer.blend");
  BLI_assert(copybuffer.is_valid());
  copybuffer.write_as_copypaste_buffer(filepath, reports);
}

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

/* TODO: copied from another PR. Refactor once that lands. */
static Vector<AnimTransformable> selected_transformables_from_context(bContext *C)
{
  Vector<AnimTransformable> transformables;
  Vector<PointerRNA> pointers;
  switch (CTX_data_mode_enum(C)) {

    case CTX_MODE_POSE: {
      CTX_data_selected_pose_bones(C, &pointers);
      for (PointerRNA &ptr : pointers) {
        transformables.append(
            {*id_cast<Object *>(ptr.owner_id), *static_cast<bPoseChannel *>(ptr.data)});
      }
      break;
    }

    default:
      break;
  }
  return transformables;
}

static wmOperatorStatus world_space_copy_exec(bContext *C, wmOperator *op)
{
  Vector<AnimTransformable> transformables = selected_transformables_from_context(C);

  Bounds<int> bounds = {RNA_int_get(op->ptr, "start"), RNA_int_get(op->ptr, "end")};
  if (bounds.is_empty()) {
    BKE_reportf(op->reports, RPT_WARNING, "Invalid frame range %d-%d", bounds.min, bounds.max);
    return OPERATOR_CANCELLED;
  }
  copy_world_space(*CTX_data_main(C),
                   *CTX_data_scene(C),
                   *CTX_data_view_layer(C),
                   *op->reports,
                   transformables,
                   bounds);
  return OPERATOR_FINISHED;
}

static bool world_space_copy_poll(bContext *C)
{
  return ED_operator_posemode(C);
}

void ANIM_OT_world_space_copy(wmOperatorType *ot)
{
  ot->name = "Copy World Space";
  ot->idname = "ANIM_OT_world_space_copy";
  ot->description = "Copy animation from selected elements to the clipboard";

  ot->exec = world_space_copy_exec;
  ot->poll = world_space_copy_poll;

  /* No undo possible since this creates data outside the current blend file. */
  ot->flag = OPTYPE_REGISTER;

  RNA_def_int(
      ot->srna, "start", 0, -INT_MAX, INT_MAX, "Start", "Start frame to copy from", 0, INT_MAX);
  RNA_def_int(
      ot->srna, "end", 250, -INT_MAX, INT_MAX, "End", "End frame to copy from", 0, INT_MAX);
}

static wmOperatorStatus world_space_paste_exec(bContext *C, wmOperator *op)
{
  return OPERATOR_FINISHED;
}

static bool world_space_paste_poll(bContext *C)
{
  return ED_operator_posemode(C);
}

void ANIM_OT_world_space_paste(wmOperatorType *ot)
{
  ot->name = "Paste World Space";
  ot->idname = "ANIM_OT_world_space_paste";
  ot->description = "Paste the animation from the clipboard to selected elements";

  ot->exec = world_space_paste_exec;
  ot->poll = world_space_paste_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::animrig
