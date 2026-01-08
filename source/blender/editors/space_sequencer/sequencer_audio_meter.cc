/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spseq
 */

#include <cmath>

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_scene.hh"
#include "BKE_scene_runtime.hh"
#include "BKE_sound.hh"

#include "BLF_api.hh"

#include "DNA_sound_types.h"
#include "DNA_space_types.h"

#include "BLI_assert.h"
#include "BLI_listbase.h"

#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "GPU_compute.hh"
#include "GPU_debug.hh"
#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_primitive.hh"
#include "GPU_shader_shared.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"

#ifdef WITH_AUDASPACE

#  include "AUD_Sequence.h"
#  include "AUD_Sound.h"
#  include "AUD_Special.h"
#  include "AUD_Types.h"

#endif

#include "SEQ_sequencer.hh"

#include "WM_api.hh"

#include "sequencer_intern.hh"
#include "sequencer_scopes.hh"

namespace blender::ed::vse {

void AudioMeter::reset()
{
  rms_left = 0.0f;
  rms_right = 0.0f;
  peak_left = 0.0f;
  peak_right = 0.0f;
}

static void draw_audio_meter_bar(float x, float y, float width, float height, float level)
{
  static const float yellow_zone_start_level = AudioMeter::dB_to_normalized(-15.0f);
  static const float red_zone_start_level = AudioMeter::dB_to_normalized(-6.0f);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Background */
  immUniformColor4f(0.1f, 0.1f, 0.1f, 0.8f);
  immRectf(pos, x, y, x + width, y + height);

  /* Border */
  immUniformColor4f(0.5f, 0.5f, 0.5f, 1.0f);
  imm_draw_box_wire_2d(pos, x, y, x + width, y + height);

  float level_height = height * level;
  float yellow_zone_start_height = height * yellow_zone_start_level;
  float red_zone_start_height = height * red_zone_start_level;

  immUniformColor4f(0.0f, 0.8f, 0.0f, 1.0f);
  immRectf(pos, x, y, x + width, y + fminf(level_height, yellow_zone_start_height));

  if (level >= yellow_zone_start_level) {
    immUniformColor4f(0.8f, 0.8f, 0.0f, 1.0f);
    immRectf(pos,
             x,
             y + yellow_zone_start_height,
             x + width,
             y + fminf(level_height, red_zone_start_height));
  }

  if (level >= red_zone_start_level) {
    immUniformColor4f(0.8f, 0.0f, 0.0f, 1.0f);
    immRectf(pos, x, y + red_zone_start_height, x + width, y + level_height);
  }

  const float spacing_between_marks = 1.5f;
  const int marks_count = level_height / spacing_between_marks;

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(0.05f);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.35f);
  immBegin(GPU_PRIM_LINES, marks_count * 2);

  float y_mark = y + spacing_between_marks;

  for (int i = 0; i < marks_count; i++) {

    immVertex2f(pos, x, y_mark);
    immVertex2f(pos, x + width, y_mark);

    y_mark += spacing_between_marks;
  }
  immEnd();
  GPU_blend(GPU_BLEND_NONE);

  immUnbindProgram();
}

static void draw_scales(float x, float y, float height)
{
  const int fontid = BLF_default();
  BLF_size(fontid, 10.0f * UI_SCALE_FAC);
  BLF_color4f(fontid, 1.0f, 1.0f, 1.0f, 0.5f);

  const char *empty = "0";
  int empty_width = BLF_width(fontid, empty, sizeof(empty));

  const char *text_50 = "-50";
  int text_height = BLF_height(fontid, text_50, sizeof(text_50));
  BLF_position(
      fontid, x, y + height * AudioMeter::dB_to_normalized(-50.0f) - 0.5f * text_height, 0.0f);
  BLF_draw(fontid, text_50, sizeof(text_50));

  const char *text_40 = "-40";
  BLF_position(
      fontid, x, y + height * AudioMeter::dB_to_normalized(-40.0f) - 0.5f * text_height, 0.0f);
  BLF_draw(fontid, text_40, sizeof(text_40));

  const char *text_30 = "-30";
  BLF_position(
      fontid, x, y + height * AudioMeter::dB_to_normalized(-30.0f) - 0.5f * text_height, 0.0f);
  BLF_draw(fontid, text_30, sizeof(text_30));

  const char *text_20 = "-20";
  BLF_position(
      fontid, x, y + height * AudioMeter::dB_to_normalized(-20.0f) - 0.5f * text_height, 0.0f);
  BLF_draw(fontid, text_20, sizeof(text_20));

  const char *text_15 = "-15";
  BLF_position(
      fontid, x, y + height * AudioMeter::dB_to_normalized(-15.0f) - 0.5f * text_height, 0.0f);
  BLF_draw(fontid, text_15, sizeof(text_15));

  const char *text_10 = "-10";
  BLF_position(
      fontid, x, y + height * AudioMeter::dB_to_normalized(-10.0f) - 0.5f * text_height, 0.0f);
  BLF_draw(fontid, text_10, sizeof(text_10));

  const char *text_5 = "-5";
  BLF_position(fontid,
               x + empty_width,
               y + height * AudioMeter::dB_to_normalized(-5.0f) - 0.5f * text_height,
               0.0f);
  BLF_draw(fontid, text_5, sizeof(text_5));

  const char *text_0 = "0";
  BLF_position(fontid,
               x + 2 * empty_width,
               y + height * AudioMeter::dB_to_normalized(0.0f) - 0.5f * text_height,
               0.0f);
  BLF_draw(fontid, text_0, sizeof(text_0));
}

static Vector<float> read_scene_sound_samples(Scene *scene, int &length_out, int &channels_out)
{
#ifdef WITH_AUDASPACE

  AUD_Sound *scene_sound = (AUD_Sound *)scene->runtime->audio.sound_scene;

  if (!scene_sound) {
    length_out = 0;
    channels_out = 0;
    return {};
  }

  AUD_Specs specs = AUD_Sequence_getSpecs(scene_sound);
  const int current_frame = scene->r.cfra;
  const int start_frame = scene->r.sfra;
  const int sample_rate = specs.rate;
  const double fps = AUD_Sequence_getFPS(scene_sound);
  const double samples_per_frame = sample_rate / fps;
  const int sample_start = static_cast<const int>((current_frame - start_frame) *
                                                  samples_per_frame);

  int samples_count = static_cast<int>(samples_per_frame);

  Vector<float> buffer(static_cast<int>(samples_per_frame * specs.channels));
  length_out = AUD_Sequence_read(
      scene_sound, sample_start, samples_count, (sample_t *)buffer.data());

  channels_out = specs.channels;
  return std::move(buffer);

#else

  length_out = 0;
  channels_out = 0;
  UNUSED_VARS(scene);
  return {};

#endif
}

static void calculate_audio_rms(Vector<float> &samples,
                                float &rms_left_out,
                                float &rms_right_out,
                                const int sample_count,
                                const int channels)
{
  if (sample_count <= 0 || sample_count > samples.size()) {
    return;
  }

  const int total_samples = sample_count * channels;
  float sum_squares_left = 0.0f;
  float sum_squares_right = 0.0f;

  switch (channels) {
    case SOUND_CHANNELS_STEREO: {
      for (int i = 0; i + 1 < total_samples; i += 2) {
        sum_squares_left += samples[i] * samples[i];
        sum_squares_right += samples[i + 1] * samples[i + 1];
      }
      break;
    }

    case SOUND_CHANNELS_MONO:
    default: {
      for (int i = 0; i < total_samples; i++) {
        sum_squares_left += samples[i] * samples[i];
        sum_squares_right = sum_squares_left;
      }
      break;
    }
  }

  const int samples_per_channel = channels == SOUND_CHANNELS_STEREO ? total_samples / 2 :
                                                                      total_samples;

  rms_left_out = sqrtf(sum_squares_left / samples_per_channel);
  rms_left_out = fminf(rms_left_out, 1.0f);

  rms_right_out = sqrtf(sum_squares_right / samples_per_channel);
  rms_right_out = fminf(rms_right_out, 1.0f);
}

static void calculate_audio_peak(Vector<float> &samples,
                                 float &peak_left_out,
                                 float &peak_right_out,
                                 const int sample_count,
                                 const int channels)
{
  if (sample_count <= 0 || sample_count > samples.size()) {
    return;
  }

  const int total_samples = sample_count * channels;
  peak_left_out = 0.0f;
  peak_right_out = 0.0f;

  switch (channels) {
    case SOUND_CHANNELS_STEREO: {
      for (int i = 0; i + 1 < total_samples; i += 2) {
        peak_left_out = fmaxf(fabsf(samples[i]), peak_left_out);
        peak_right_out = fmaxf(fabsf(samples[i + 1]), peak_right_out);
      }
      break;
    }

    case SOUND_CHANNELS_MONO:
    default: {
      for (int i = 0; i < total_samples; i++) {
        peak_left_out = fmaxf(fabsf(samples[i]), peak_left_out);
      }
      peak_right_out = peak_left_out;
      break;
    }
  }

  peak_left_out = fminf(peak_left_out, 1.0f);
  peak_right_out = fminf(peak_right_out, 1.0f);
}

void AudioMeter::update_audio_meter_data(const bContext * /*C*/, Scene *scene)
{
  int sample_count = 0;
  int channels = 0;
  Vector<float> samples = read_scene_sound_samples(scene, sample_count, channels);

  if (sample_count <= 0 || channels <= 0) {
    this->reset();
    return;
  }

  calculate_audio_rms(samples, this->rms_left, this->rms_right, sample_count, channels);
  calculate_audio_peak(samples, this->peak_left, this->peak_right, sample_count, channels);
}

void draw_audio_meter(const bContext *C, ARegion * /*region*/)
{
  ScrArea *area = CTX_wm_area(C);
  SpaceSeq *sseq = static_cast<SpaceSeq *>(area->spacedata.first);
  Scene *scene = CTX_data_sequencer_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer);
  Scene *scene_eval = (depsgraph != nullptr) ? DEG_get_evaluated_scene(depsgraph) : nullptr;

  AudioMeter *audio_meter = &sseq->runtime->scopes.audio_meter;
  const bool is_playing = ED_screen_animation_playing(CTX_wm_manager(C));
  bool is_playing_reverse = false;

  bScreen *screen = CTX_wm_screen(C);
  wmTimer *wt = screen->animtimer;
  if (wt) {
    ScreenAnimData *sad = static_cast<ScreenAnimData *>(wt->customdata);
    is_playing_reverse = (sad->flag & ANIMPLAY_FLAG_REVERSE);
  }

  if (G.is_rendering || !scene_eval || !scene_eval->ed || !is_playing || is_playing_reverse) {
    audio_meter->reset();
  }
  else {
    audio_meter->update_audio_meter_data(C, scene_eval);
  }

  float rms_left_level = AudioMeter::dB_to_normalized(
      AudioMeter::linear_to_dB(audio_meter->rms_left));
  float rms_right_level = AudioMeter::dB_to_normalized(
      AudioMeter::linear_to_dB(audio_meter->rms_right));

  UNUSED_VARS(rms_left_level, rms_right_level);

  float peak_left_level = AudioMeter::dB_to_normalized(
      AudioMeter::linear_to_dB(audio_meter->peak_left));
  float peak_right_level = AudioMeter::dB_to_normalized(
      AudioMeter::linear_to_dB(audio_meter->peak_right));

  draw_scales(4.0f, 10.0f, 300.0f);
  draw_audio_meter_bar(27.0f, 10.0f, 12.0f, 300.0f, peak_left_level);
  draw_audio_meter_bar(39.0f, 10.0f, 12.0f, 300.0f, peak_right_level);
}

}  // namespace blender::ed::vse
