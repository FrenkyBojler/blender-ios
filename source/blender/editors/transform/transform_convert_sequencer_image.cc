/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "BLI_index_range.hh"
#include "MEM_guardedalloc.h"

#include "DNA_sequence_types.h"
#include "DNA_space_types.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "SEQ_channels.hh"
#include "SEQ_iterator.hh"
#include "SEQ_relations.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_transform.hh"

#include "ANIM_keyframing.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include <array>

using namespace blender;

/** Used for sequencer transform. */
struct TransDataSeq {
  Strip *strip;
  std::array<float2, 4> quad_orig;
  float4x4 orig_matrix;

  float orig_origin_relative[2];
  float orig_origin_position[2];
  float orig_translation[2];
  float orig_scale[2];
  float orig_rotation;
};

static TransData *SeqToTransData(const Scene *scene,
                                 Strip *strip,
                                 TransData *td,
                                 TransData2D *td2d,
                                 TransDataSeq *tdseq,
                                 int vert_index)
{
  const StripTransform *transform = strip->data->transform;
  float origin[2];
  SEQ_image_transform_origin_offset_pixelspace_get(scene, strip, origin);
  float vertex[2] = {origin[0], origin[1]};

  /* Add control vertex, so rotation and scale can be calculated.
   * All three vertices will form a "L" shape that is aligned to the local strip axis.
   */
  if (vert_index == 1) {
    vertex[0] += cosf(transform->rotation);
    vertex[1] += sinf(transform->rotation);
  }
  else if (vert_index == 2) {
    vertex[0] -= sinf(transform->rotation);
    vertex[1] += cosf(transform->rotation);
  }

  td2d->loc[0] = vertex[0];
  td2d->loc[1] = vertex[1];
  td2d->loc2d = nullptr;
  td->loc = td2d->loc;
  copy_v3_v3(td->iloc, td->loc);

  td->center[0] = origin[0];
  td->center[1] = origin[1];

  unit_m3(td->mtx);
  unit_m3(td->smtx);

  axis_angle_to_mat3_single(td->axismtx, 'Z', transform->rotation);
  normalize_m3(td->axismtx);

  // if (vert_index == 0) {
  tdseq->strip = strip;
  copy_v2_v2(tdseq->orig_origin_relative, transform->origin);
  copy_v2_v2(tdseq->orig_origin_position, origin);
  tdseq->quad_orig = SEQ_image_transform_final_quad_get(scene, strip);
  tdseq->orig_matrix = math::invert(SEQ_image_transform_matrix_get(scene, strip));

  tdseq->orig_translation[0] = transform->xofs;
  tdseq->orig_translation[1] = transform->yofs;
  tdseq->orig_scale[0] = transform->scale_x;
  tdseq->orig_scale[1] = transform->scale_y;
  tdseq->orig_rotation = transform->rotation;
  //}

  td->extra = (void *)tdseq;
  td->ext = nullptr;
  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  return td;
}

static void freeSeqData(TransInfo * /*t*/,
                        TransDataContainer *tc,
                        TransCustomData * /*custom_data*/)
{
  TransData *td = tc->data;
  MEM_freeN(td->extra);
}

static void createTransSeqImageData(bContext * /*C*/, TransInfo *t)
{
  Editing *ed = SEQ_editing_get(t->scene);
  const SpaceSeq *sseq = static_cast<const SpaceSeq *>(t->area->spacedata.first);
  const ARegion *region = t->region;

  if (ed == nullptr) {
    return;
  }
  if (sseq->mainb != SEQ_DRAW_IMG_IMBUF) {
    return;
  }
  if (region->regiontype == RGN_TYPE_PREVIEW && sseq->view == SEQ_VIEW_SEQUENCE_PREVIEW) {
    return;
  }

  ListBase *seqbase = SEQ_active_seqbase_get(ed);
  ListBase *channels = SEQ_channels_displayed_get(ed);
  VectorSet strips = SEQ_query_rendered_strips(t->scene, channels, seqbase, t->scene->r.cfra, 0);
  strips.remove_if([&](Strip *strip) { return (strip->flag & SELECT) == 0; });

  if (strips.is_empty()) {
    return;
  }

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tc->custom.type.free_cb = freeSeqData;

  tc->data_len = strips.size() * 3; /* 3 vertices per sequence are needed. */
  TransData *td = tc->data = static_cast<TransData *>(
      MEM_callocN(tc->data_len * sizeof(TransData), "TransSeq TransData"));
  TransData2D *td2d = tc->data_2d = static_cast<TransData2D *>(
      MEM_callocN(tc->data_len * sizeof(TransData2D), "TransSeq TransData2D"));
  TransDataSeq *tdseq = static_cast<TransDataSeq *>(
      MEM_callocN(tc->data_len * sizeof(TransDataSeq), "TransSeq TransDataSeq"));

  for (Strip *strip : strips) {
    /* One `Sequence` needs 3 `TransData` entries - center point placed in image origin, then 2
     * points offset by 1 in X and Y direction respectively, so rotation and scale can be
     * calculated from these points. */
    SeqToTransData(t->scene, strip, td++, td2d++, tdseq++, 0);
    SeqToTransData(t->scene, strip, td++, td2d++, tdseq++, 1);
    SeqToTransData(t->scene, strip, td++, td2d++, tdseq++, 2);
  }
}

static bool autokeyframe_sequencer_image(bContext *C,
                                         Scene *scene,
                                         StripTransform *transform,
                                         const int tmode)
{
  PropertyRNA *prop;
  PointerRNA ptr = RNA_pointer_create_discrete(&scene->id, &RNA_StripTransform, transform);

  const bool around_cursor = scene->toolsettings->sequencer_tool_settings->pivot_point ==
                             V3D_AROUND_CURSOR;
  const bool do_loc = tmode == TFM_TRANSLATION || around_cursor;
  const bool do_rot = tmode == TFM_ROTATION;
  const bool do_scale = tmode == TFM_RESIZE;
  const bool only_when_keyed = animrig::is_keying_flag(scene, AUTOKEY_FLAG_INSERTAVAILABLE);

  bool changed = false;
  if (do_rot) {
    prop = RNA_struct_find_property(&ptr, "rotation");
    changed |= animrig::autokeyframe_property(
        C, scene, &ptr, prop, -1, scene->r.cfra, only_when_keyed);
  }
  if (do_loc) {
    prop = RNA_struct_find_property(&ptr, "offset_x");
    changed |= animrig::autokeyframe_property(
        C, scene, &ptr, prop, -1, scene->r.cfra, only_when_keyed);
    prop = RNA_struct_find_property(&ptr, "offset_y");
    changed |= animrig::autokeyframe_property(
        C, scene, &ptr, prop, -1, scene->r.cfra, only_when_keyed);
  }
  if (do_scale) {
    prop = RNA_struct_find_property(&ptr, "scale_x");
    changed |= animrig::autokeyframe_property(
        C, scene, &ptr, prop, -1, scene->r.cfra, only_when_keyed);
    prop = RNA_struct_find_property(&ptr, "scale_y");
    changed |= animrig::autokeyframe_property(
        C, scene, &ptr, prop, -1, scene->r.cfra, only_when_keyed);
  }

  return changed;
}

struct TransformData {
  float2 origin;
  float2 handle_x;
  float2 handle_y;
};

static TransformData transform_data_get(TransData2D *td2d)
{
  TransformData data;
  /* Origin. */
  data.origin = {td2d->loc[0], td2d->loc[1]};
  /* X and Y control points used to read scale and rotation. */
  data.handle_x = float2((td2d + 1)->loc) - data.origin;
  data.handle_y = float2((td2d + 2)->loc) - data.origin;
  return data;
}

static float3 transform_translation_get(TransInfo *t,
                                        TransDataSeq *tdseq,
                                        TransData2D *td2d,
                                        Strip *strip)
{
  TransformData data = transform_data_get(td2d);
  float2 mirror;
  SEQ_image_transform_mirror_factor_get(strip, mirror);
  // float3 translation = (float3(tdseq->orig_origin_position) - data.origin) * float3(mirror);
  float3 translation = {(tdseq->orig_origin_position[0] - data.origin.x) * mirror.x,
                        (tdseq->orig_origin_position[1] - data.origin.y) * mirror.y,
                        0.0f};
  translation[0] *= t->scene->r.yasp / t->scene->r.xasp;
  return translation;
}

static void image_transform_set(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  TransData *td = nullptr;
  TransData2D *td2d = nullptr;
  int i;

  for (i = 0, td = tc->data, td2d = tc->data_2d; i < tc->data_len; i += 3, td += 3, td2d += 3) {
    TransDataSeq *tdseq = static_cast<TransDataSeq *>(td->extra);
    Strip *strip = tdseq->strip;
    StripTransform *transform = strip->data->transform;

    /* Calculate translation. */
    float3 translation = transform_translation_get(t, tdseq, td2d, strip);

    /* Round resulting position to integer pixels. Resulting strip
     * will more often end up using faster interpolation (without bilinear),
     * and avoids "text edges are too dark" artifacts with light text strips
     * on light backgrounds. The latter happens because bilinear filtering
     * does not do full alpha pre-multiplication. */
    transform->xofs = roundf(tdseq->orig_translation[0] - translation[0]);
    transform->yofs = roundf(tdseq->orig_translation[1] - translation[1]);

    /* Scale. */
    TransformData data = transform_data_get(td2d);
    transform->scale_x = tdseq->orig_scale[0] * fabs(math::length(data.handle_x));
    transform->scale_y = tdseq->orig_scale[1] * fabs(math::length(data.handle_y));

    /* Rotation. Scaling can cause negative rotation. */
    if (t->mode == TFM_ROTATION) {
      transform->rotation = tdseq->orig_rotation - t->values_final[0];
    }

    if ((t->animtimer) && animrig::is_autokey_on(t->scene)) {
      animrecord_check_state(t, &t->scene->id);
      autokeyframe_sequencer_image(t->context, t->scene, transform, t->mode);
    }

    SEQ_relations_invalidate_cache_preprocessed(t->scene, strip);
  }
}

static float2 calculate_translation_offset(TransInfo *t, TransDataSeq *tdseq)
{
  Strip *strip = tdseq->strip;
  StripTransform *transform = strip->data->transform;

  /* During modal operation, transform->*ofs is adjusted. Reset this value to original state, so
   * that new offset can be calculated. */
  transform->xofs = tdseq->orig_translation[0];
  transform->yofs = tdseq->orig_translation[1];

  const float2 viewport_pixel_aspect = {t->scene->r.xasp / t->scene->r.yasp, 1.0f};
  float2 mirror;
  SEQ_image_transform_mirror_factor_get(strip, mirror);

  std::array<float2, 4> quad_new = SEQ_image_transform_final_quad_get(t->scene, strip);
  return (quad_new[0] - tdseq->quad_orig[0]) * mirror / viewport_pixel_aspect;
}

static float2 calculate_new_origin_position(TransInfo *t, TransDataSeq *tdseq, TransData2D *td2d)
{
  Strip *strip = tdseq->strip;

  float3 image_size(float(t->scene->r.xsch), float(t->scene->r.ysch), 0.0f);
  if (ELEM(strip->type, STRIP_TYPE_MOVIE, STRIP_TYPE_IMAGE)) {
    image_size.x = strip->data->stripdata->orig_width;
    image_size.y = strip->data->stripdata->orig_height;
  }

  const float3 viewport_pixel_aspect = {t->scene->r.xasp / t->scene->r.yasp, 1.0f, 1.0f};
  float2 mirror;
  SEQ_image_transform_mirror_factor_get(strip, mirror);

  const float3 origin = {tdseq->orig_origin_position[0], tdseq->orig_origin_position[1], 0.0f};
  const float3 translation = transform_translation_get(t, tdseq, td2d, strip);
  const float3 origin_pixelspace_unscaled = {(origin.x / viewport_pixel_aspect.x) * mirror.x,
                                             (origin.y / viewport_pixel_aspect.y) * mirror.y,
                                             0.0f};
  const float3 origin_translated = origin_pixelspace_unscaled - translation;
  const float3 origin_raw_space = math::transform_point(tdseq->orig_matrix, origin_translated);
  const float3 origin_abs = origin_raw_space + (image_size / 2);
  const float2 origin_rel = {origin_abs.x / image_size.x, origin_abs.y / image_size.y};
  return origin_rel;
}

static void image_origin_set(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  TransData *td = nullptr;
  TransData2D *td2d = nullptr;
  int i;

  for (i = 0, td = tc->data, td2d = tc->data_2d; i < tc->data_len; i += 3, td += 3, td2d += 3) {
    TransDataSeq *tdseq = static_cast<TransDataSeq *>(td->extra);
    Strip *strip = tdseq->strip;
    StripTransform *transform = strip->data->transform;

    const float2 origin_rel = calculate_new_origin_position(t, tdseq, td2d);
    transform->origin[0] = origin_rel.x;
    transform->origin[1] = origin_rel.y;

    /* Calculate offset, so image does not change it's position in preview. */
    float2 delta_translation = calculate_translation_offset(t, tdseq);
    transform->xofs = tdseq->orig_translation[0] - delta_translation.x;
    transform->yofs = tdseq->orig_translation[1] - delta_translation.y;

    SEQ_relations_invalidate_cache_preprocessed(t->scene, strip);
  }
}

static void recalcData_sequencer_image(TransInfo *t)
{
  if ((t->flag & T_ORIGIN) == 0) {
    image_transform_set(t);
  }
  else {
    image_origin_set(t);
  }
}

static void special_aftertrans_update__sequencer_image(bContext * /*C*/, TransInfo *t)
{

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  TransData *td = nullptr;
  TransData2D *td2d = nullptr;
  int i;

  for (i = 0, td = tc->data, td2d = tc->data_2d; i < tc->data_len; i += 3, td += 3, td2d += 3) {
    TransDataSeq *tdseq = static_cast<TransDataSeq *>(td->extra);
    Strip *strip = tdseq->strip;
    StripTransform *transform = strip->data->transform;
    if (t->state == TRANS_CANCEL) {
      transform->xofs = tdseq->orig_translation[0];
      transform->yofs = tdseq->orig_translation[1];
      transform->rotation = tdseq->orig_rotation;
      transform->scale_x = tdseq->orig_scale[0];
      transform->scale_y = tdseq->orig_scale[1];
      transform->origin[0] = tdseq->orig_origin_relative[0];
      transform->origin[1] = tdseq->orig_origin_relative[1];
    }

    if (animrig::is_autokey_on(t->scene)) {
      autokeyframe_sequencer_image(t->context, t->scene, transform, t->mode);
    }
  }
}

TransConvertTypeInfo TransConvertType_SequencerImage = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransSeqImageData,
    /*recalc_data*/ recalcData_sequencer_image,
    /*special_aftertrans_update*/ special_aftertrans_update__sequencer_image,
};
