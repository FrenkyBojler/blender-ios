/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_action.hh"
#include "BKE_appdir.hh"
#include "BKE_blendfile.hh"
#include "BKE_fcurve.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_anim_transformable.hh"

#include "ANIM_action.hh"

namespace blender::ed {

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
  bke::blendfile::PartialWriteContext copybuffer{bmain};
  bAction *dna_action = reinterpret_cast<bAction *>(copybuffer.id_create(
      ID_AC,
      "world_space_copy",
      nullptr,
      {(bke::blendfile::PartialWriteContext::IDAddOperations::SET_FAKE_USER |
        bke::blendfile::PartialWriteContext::IDAddOperations::SET_CLIPBOARD_MARK)}));

  animrig::Action &action = dna_action->wrap();
  action.layer_keystrip_ensure();
  animrig::StripKeyframeData &strip_data =
      action.layer(0)->strip(0)->data<animrig::StripKeyframeData>(action);
  animrig::Slot &slot = action.slot_add();
  animrig::Channelbag &channelbag = strip_data.channelbag_for_slot_ensure(slot);

  /* We are storing the world space matrix in separate FCurves so the data can be stored in a
   * blend file. */
  Vector<ID *> ids;
  Map<const AnimTransformable *, Array<FCurve *>> world_space_data;
  for (const AnimTransformable &transformable : transformables) {
    Array<FCurve *> fcurves(12);
    if (!world_space_data.add(&transformable, fcurves)) {
      BLI_assert_unreachable();
      continue;
    }
    ids.append(transformable.owner_id());
    for (const int i : fcurves.index_range()) {
      FCurve *fcurve = BKE_fcurve_create();
      fcurve->rna_path = BLI_strdupn(transformable.name().data(), transformable.name().size());
      fcurve->array_index = i;
      /* Using FPoint because we only need 2 floats per key, not the huge struct that
       * is BezTriple.  */
      fcurve->fpt = MEM_new_array_uninitialized<FPoint>(range.size(), "world_space_copy_points");
      /* Could allocate space on the channelbag in big chunks instead of appending which is a
       * MEM_new every time. */
      channelbag.fcurve_append(*fcurve);
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

}  // namespace blender::ed
