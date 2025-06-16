/* SPDX-FileCopyrightText: 2011 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spclip
 */

#include "DNA_movieclip_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BLI_utildefines.h"

#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "ED_anim_api.hh"
#include "ED_clip.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "clip_intern.hh" /* own include */

struct TrackMotionCurveUserData {
  SpaceClip *sc;
  MovieTrackingTrack *act_track;
  bool sel;
  float xscale, yscale, hsize;
  uint pos;
  /** Current bound shader. */
  std::optional<eGPUBuiltinShader> active_shader;
};

static void tracking_segment_point_cb(void *userdata,
                                      MovieTrackingTrack * /*track*/,
                                      MovieTrackingMarker * /*marker*/,
                                      eClipCurveValueSource value_source,
                                      int scene_framenr,
                                      float val)
{
  TrackMotionCurveUserData *data = (TrackMotionCurveUserData *)userdata;

  if (!clip_graph_value_visible(data->sc, value_source)) {
    return;
  }

  immVertex2f(data->pos, scene_framenr, val);
}

static void tracking_segment_start_cb(void *userdata,
                                      MovieTrackingTrack *track,
                                      eClipCurveValueSource value_source,
                                      bool is_point)
{
  TrackMotionCurveUserData *data = (TrackMotionCurveUserData *)userdata;
  SpaceClip *sc = data->sc;
  ClipShaderState shader_data = {is_point ? GPU_SHADER_3D_POINT_UNIFORM_COLOR :
                                            GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR,
                                 3.0f,
                                 0.0,
                                 blender::float4()};

  if (!clip_graph_value_visible(sc, value_source)) {
    return;
  }

  switch (value_source) {
    case CLIP_VALUE_SOURCE_SPEED_X:
      shader_data.color.x = 1.0f;
      break;
    case CLIP_VALUE_SOURCE_SPEED_Y:
      shader_data.color.y = 1.0f;
      break;
    case CLIP_VALUE_SOURCE_REPROJECTION_ERROR:
      shader_data.color.z = 1.0f;
      break;
  }

  if (track == data->act_track) {
    shader_data.color.w = 1.0f;
    shader_data.line_width = U.pixelsize * 2.0f;
  }
  else {
    shader_data.color.w = 0.5f;
    shader_data.line_width = U.pixelsize * 1.0f;
  }

  clip_ensure_shader(data->active_shader, shader_data);
  if (is_point) {
    immBeginAtMost(GPU_PRIM_POINTS, 1);
  }
  else {
    /* Graph can be composed of smaller segments, if any marker is disabled */
    immBeginAtMost(GPU_PRIM_LINE_STRIP, track->markersnr);
  }
}

static void tracking_segment_end_cb(void *userdata, eClipCurveValueSource value_source)
{
  TrackMotionCurveUserData *data = (TrackMotionCurveUserData *)userdata;
  SpaceClip *sc = data->sc;
  if (!clip_graph_value_visible(sc, value_source)) {
    return;
  }
  immEnd();
}

static void tracking_segment_knot_cb(void *userdata,
                                     MovieTrackingTrack *track,
                                     MovieTrackingMarker *marker,
                                     eClipCurveValueSource value_source,
                                     int scene_framenr,
                                     float val)
{
  TrackMotionCurveUserData *data = (TrackMotionCurveUserData *)userdata;

  if (track != data->act_track) {
    return;
  }
  if (!ELEM(value_source, CLIP_VALUE_SOURCE_SPEED_X, CLIP_VALUE_SOURCE_SPEED_Y)) {
    return;
  }

  const int sel_flag = value_source == CLIP_VALUE_SOURCE_SPEED_X ? MARKER_GRAPH_SEL_X :
                                                                   MARKER_GRAPH_SEL_Y;
  const bool sel = (marker->flag & sel_flag) != 0;

  if (sel == data->sel) {
    ClipShaderState shader_data = {
        GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR,
        0.0f,
        1.0f,

    };
    UI_GetThemeColor4fv(sel ? TH_HANDLE_VERTEX_SELECT : TH_HANDLE_VERTEX, shader_data.color);
    clip_ensure_shader(data->active_shader, shader_data);

    GPU_matrix_push();
    GPU_matrix_translate_2f(scene_framenr, val);
    GPU_matrix_scale_2f(1.0f / data->xscale * data->hsize, 1.0f / data->yscale * data->hsize);

    imm_draw_circle_wire_2d(data->pos, 0, 0, 0.7, 8);

    GPU_matrix_pop();
  }
}

static void draw_tracks_motion_and_error_curves(SpaceClip *sc, TrackMotionCurveUserData &data)
{
  MovieClip *clip = ED_space_clip_get_clip(sc);
  const bool draw_knots = (sc->flag & SC_SHOW_GRAPH_TRACKS_MOTION) != 0;

  int width, height;
  BKE_movieclip_get_size(clip, &sc->user, &width, &height);
  if (!width || !height) {
    return;
  }

  data.sel = false;
  /* Non-selected knot handles. */
  if (draw_knots) {
    clip_graph_tracking_values_iterate(sc,
                                       (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                       (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                       &data,
                                       tracking_segment_knot_cb,
                                       nullptr,
                                       nullptr);
  }

  /* Draw graph lines. */
  GPU_blend(GPU_BLEND_ALPHA);
  clip_graph_tracking_values_iterate(sc,
                                     (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                     (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                     &data,
                                     tracking_segment_point_cb,
                                     tracking_segment_start_cb,
                                     tracking_segment_end_cb);
  GPU_blend(GPU_BLEND_NONE);

  /* Selected knot handles on top of curves. */
  if (draw_knots) {
    data.sel = true;
    clip_graph_tracking_values_iterate(sc,
                                       (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                       (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                       &data,
                                       tracking_segment_knot_cb,
                                       nullptr,
                                       nullptr);
  }
}

static void draw_frame_curves(SpaceClip *sc, uint pos, TrackMotionCurveUserData &data)
{
  MovieClip *clip = ED_space_clip_get_clip(sc);
  const MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(&clip->tracking);
  const MovieTrackingReconstruction *reconstruction = &tracking_object->reconstruction;

  int previous_frame;
  float previous_error;
  bool have_previous_point = false;

  /* Indicates whether immBegin() was called. */
  bool is_lines_segment_open = false;

  ClipShaderState shader_data = {
      GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR, 1.0f, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}};
  clip_ensure_shader(data.active_shader, shader_data);

  for (int i = 0; i < reconstruction->camnr; i++) {
    MovieReconstructedCamera *camera = &reconstruction->cameras[i];

    const int current_frame = BKE_movieclip_remap_clip_to_scene_frame(clip, camera->framenr);
    const float current_error = camera->error;

    if (have_previous_point && current_frame != previous_frame + 1) {
      if (is_lines_segment_open) {
        immEnd();
        is_lines_segment_open = false;
      }
      have_previous_point = false;
    }

    if (have_previous_point) {
      if (!is_lines_segment_open) {
        immBeginAtMost(GPU_PRIM_LINE_STRIP, reconstruction->camnr);
        is_lines_segment_open = true;

        immVertex2f(pos, previous_frame, previous_error);
      }
      immVertex2f(pos, current_frame, current_error);
    }

    previous_frame = current_frame;
    previous_error = current_error;
    have_previous_point = true;
  }

  if (is_lines_segment_open) {
    immEnd();
  }
}

void clip_draw_graph(SpaceClip *sc, ARegion *region, Scene *scene)
{
  MovieClip *clip = ED_space_clip_get_clip(sc);
  View2D *v2d = &region->v2d;

  /* grid */
  UI_view2d_draw_lines_x__values(v2d);
  UI_view2d_draw_lines_y__values(v2d);

  if (clip) {
    uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

    const MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(&clip->tracking);
    MovieTrackingTrack *active_track = tracking_object->active_track;
    TrackMotionCurveUserData data{};
    data.sc = sc;
    data.hsize = UI_GetThemeValuef(TH_HANDLE_VERTEX_SIZE);
    data.sel = false;
    data.act_track = active_track;
    data.pos = pos;
    UI_view2d_scale_get(v2d, &data.xscale, &data.yscale);

    if (sc->flag & (SC_SHOW_GRAPH_TRACKS_MOTION | SC_SHOW_GRAPH_TRACKS_ERROR)) {
      draw_tracks_motion_and_error_curves(sc, data);
    }

    if (sc->flag & SC_SHOW_GRAPH_FRAMES) {
      draw_frame_curves(sc, pos, data);
    }

    clip_unbind_shader(data.active_shader);
  }

  /* Frame and preview range. */
  UI_view2d_view_ortho(v2d);
  ANIM_draw_framerange(scene, v2d);
  ANIM_draw_previewrange(scene, v2d, 0);
}
