#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>

#include "MEM_guardedalloc.h"

#include "BLI_fileops.h"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_sound.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "SEQ_add.hh"
#include "SEQ_edit.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_transform.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"
#include "ED_sequencer.hh"

#include "sequencer_intern.hh"

namespace blender::ed::vse {

static bool g_voiceover_stop_requested = false;

static int voiceover_countdown_get(const Editing *ed)
{
  if (ed == nullptr || ed->runtime == nullptr) {
    return 0;
  }
  return ed->runtime->voiceover_countdown;
}

static void voiceover_countdown_set(Editing *ed, const int seconds)
{
  if (ed == nullptr || ed->runtime == nullptr) {
    return;
  }
  ed->runtime->voiceover_countdown = min_ii(max_ii(seconds, 0), 255);
}

struct VoiceoverState {
  wmTimer *timer = nullptr;
  blender::SoundVoiceoverSession *session = nullptr;
  Strip *feedback_strip = nullptr;
  int initial_frame = 0;
  int record_start_frame = 0;
  int channel = 1;
  int pre_roll_seconds = 0;
  std::chrono::steady_clock::time_point pre_roll_deadline;
  bool was_playing = false;
  bool started_recording = false;
  bool muted_before = false;
  bool muted_during = false;
  char filepath[FILE_MAX] = "";
};

struct VoiceoverResolvedSettings {
  int pre_roll = 3;
  int channel = 1;
  bool mute_sound = true;
  int input_device = 0;
  float gain = 1.0f;
  char directory[FILE_MAXDIR] = "//";
  char filename[256] = "voiceover";
  int container = SEQ_EDIT_VOICEOVER_CONTAINER_WAV;
  int codec = SEQ_EDIT_VOICEOVER_CODEC_PCM;
  int audio_channels = 2;
  int sample_rate = 48000;
  int bitrate = 256;
};

static void voiceover_codec_pair_normalize(VoiceoverResolvedSettings *settings)
{
  /* Keep container/codec pairs in sync to avoid invalid writer combinations. */
  switch (settings->codec) {
    case SEQ_EDIT_VOICEOVER_CODEC_AAC:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_AAC;
      break;
    case SEQ_EDIT_VOICEOVER_CODEC_AC3:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_AC3;
      break;
    case SEQ_EDIT_VOICEOVER_CODEC_FLAC:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_FLAC;
      break;
    case SEQ_EDIT_VOICEOVER_CODEC_MP2:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_MP2;
      break;
    case SEQ_EDIT_VOICEOVER_CODEC_MP3:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_MP3;
      break;
    case SEQ_EDIT_VOICEOVER_CODEC_OPUS:
    case SEQ_EDIT_VOICEOVER_CODEC_VORBIS:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_OGG;
      break;
    case SEQ_EDIT_VOICEOVER_CODEC_PCM:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_WAV;
      break;
    default:
      settings->container = SEQ_EDIT_VOICEOVER_CONTAINER_WAV;
      settings->codec = SEQ_EDIT_VOICEOVER_CODEC_PCM;
      break;
  }
}

static void voiceover_resolve_settings(const Editing *ed, VoiceoverResolvedSettings *r_settings)
{
  if (ed == nullptr) {
    return;
  }

  r_settings->input_device = ed->voiceover_input_device;
  r_settings->gain = ed->voiceover_gain;
  r_settings->pre_roll = ed->voiceover_pre_roll;
  r_settings->channel = ed->voiceover_channel;
  r_settings->mute_sound = (ed->voiceover_mute_sound != 0);
  STRNCPY(r_settings->directory, ed->voiceover_directory);
  STRNCPY(r_settings->filename, ed->voiceover_filename);
  r_settings->container = ed->voiceover_container;
  r_settings->codec = ed->voiceover_codec;
  r_settings->audio_channels = ed->voiceover_audio_channels;
  r_settings->sample_rate = ed->voiceover_sample_rate;
  r_settings->bitrate = ed->voiceover_bitrate;

  r_settings->pre_roll = max_ii(0, r_settings->pre_roll);
  r_settings->channel = max_ii(1, r_settings->channel);
  r_settings->gain = max_ff(0.0f, r_settings->gain);
  if (r_settings->directory[0] == '\0') {
    STRNCPY(r_settings->directory, "//");
  }
  if (r_settings->filename[0] == '\0') {
    STRNCPY(r_settings->filename, "voiceover");
  }
  if (!ELEM(r_settings->audio_channels, 1, 2)) {
    r_settings->audio_channels = 2;
  }
  if (!ELEM(r_settings->sample_rate, 44100, 48000, 96000)) {
    r_settings->sample_rate = 48000;
  }
  if (r_settings->bitrate <= 0) {
    r_settings->bitrate = 256;
  }
  if (r_settings->codec < SEQ_EDIT_VOICEOVER_CODEC_AAC ||
      r_settings->codec > SEQ_EDIT_VOICEOVER_CODEC_OPUS)
  {
    r_settings->codec = SEQ_EDIT_VOICEOVER_CODEC_PCM;
  }
  voiceover_codec_pair_normalize(r_settings);
}

static bool voiceover_poll(bContext *C)
{
  SpaceSeq *sseq = CTX_wm_space_seq(C);
  if (sseq == nullptr || !ELEM(sseq->view, SEQ_VIEW_SEQUENCE, SEQ_VIEW_SEQUENCE_PREVIEW)) {
    return false;
  }
  return CTX_data_sequencer_scene(C) != nullptr;
}

static bool voiceover_filepath_generate(bContext *C,
                                        const VoiceoverResolvedSettings &settings,
                                        char r_filepath[FILE_MAX],
                                        ReportList *reports)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_sequencer_scene(C);

  if (BLI_path_is_rel(settings.directory) && BKE_main_blendfile_path(bmain)[0] == '\0') {
    BKE_report(reports,
               RPT_ERROR,
               "Save the .blend file before using a relative Voiceover output directory");
    return false;
  }

  char directory[FILE_MAXDIR];
  STRNCPY(directory, settings.directory);
  BLI_path_abs(directory, ID_BLEND_PATH(bmain, &scene->id));

  if (!BLI_is_dir(directory)) {
    if (!BLI_dir_create_recursive(directory) || !BLI_is_dir(directory)) {
      BKE_reportf(reports, RPT_ERROR, "Voiceover directory does not exist: %s", directory);
      return false;
    }
  }

  char timestamp[64];
  std::time_t raw = std::time(nullptr);
  std::tm *timeinfo = std::localtime(&raw);
  std::strftime(timestamp, sizeof(timestamp), "_%Y-%m-%d_%H-%M-%S", timeinfo);

  const char *ext = ".wav";
  if (settings.container == SEQ_EDIT_VOICEOVER_CONTAINER_AAC) {
    ext = ".aac";
  }
  else if (settings.container == SEQ_EDIT_VOICEOVER_CONTAINER_FLAC) {
    ext = ".flac";
  }
  else if (settings.container == SEQ_EDIT_VOICEOVER_CONTAINER_AC3) {
    ext = ".ac3";
  }
  else if (settings.container == SEQ_EDIT_VOICEOVER_CONTAINER_MP2) {
    ext = ".mp2";
  }
  else if (settings.container == SEQ_EDIT_VOICEOVER_CONTAINER_OGG) {
    ext = ".ogg";
  }
  else if (settings.container == SEQ_EDIT_VOICEOVER_CONTAINER_MP3) {
    ext = ".mp3";
  }

  char filename[FILE_MAXFILE];
  SNPRINTF(filename, "%s%s%s", settings.filename, timestamp, ext);
  BLI_path_join(r_filepath, FILE_MAX, directory, filename);

  if (BLI_exists(r_filepath)) {
    BKE_reportf(reports, RPT_ERROR, "Voiceover output already exists: %s", r_filepath);
    return false;
  }

  return true;
}

static bool voiceover_is_playing_current_screen(const bContext *C)
{
  const bScreen *screen = CTX_wm_screen(C);
  return (screen != nullptr) && (screen->animtimer != nullptr);
}

// voiceover: does not seem right to full deps?
static Scene *voiceover_scene_eval_get(bContext *C)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  if (scene == nullptr) {
    return nullptr;
  }

  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = BKE_view_layer_default_render(scene);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
  return DEG_get_evaluated_scene(depsgraph);
}

static bool voiceover_playback_start(bContext *C, ReportList *reports)
{
  if (voiceover_is_playing_current_screen(C)) {
    return true;
  }

  /* If another window is already playing, stop it first so we can start playback here. */
  if (ED_screen_animation_playing(CTX_wm_manager(C))) {
    ED_screen_animation_play(C, 0, 0);
  }

  /* Start forward playback. */
  ED_screen_animation_play(C, -1, 1);
  if (!voiceover_is_playing_current_screen(C)) {
    BKE_report(reports, RPT_ERROR, "Failed to start playback for voiceover recording");
    return false;
  }
  return true;
}

static bool voiceover_feedback_strip_add(bContext *C, VoiceoverState *state)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  Editing *ed = seq::editing_ensure(scene);
  seq::LoadData load_data{};
  load_data.start_frame = state->record_start_frame;
  load_data.channel = state->channel;
  load_data.effect.type = STRIP_TYPE_COLOR;
  load_data.effect.length = 1;
  STRNCPY(load_data.name, "Recording");

  state->feedback_strip = seq::add_effect_strip(scene, ed->current_strips(), &load_data);
  if (state->feedback_strip == nullptr) {
    return false;
  }

  seq::transform_seqbase_shuffle(ed->current_strips(), state->feedback_strip, scene);
  state->channel = state->feedback_strip->channel;
  /* Keep feedback strip visible while recording. */
  state->feedback_strip->blend_opacity = 100.0f;
  return true;
}

static void voiceover_feedback_strip_remove(Scene *scene, VoiceoverState *state)
{
  if (state->feedback_strip == nullptr || scene->ed == nullptr) {
    return;
  }
  seq::edit_flag_for_removal(scene, scene->ed->current_strips(), state->feedback_strip);
  seq::edit_remove_flagged_strips(scene, scene->ed->current_strips());
  state->feedback_strip = nullptr;
}

static void voiceover_sound_strip_add(bContext *C, VoiceoverState *state)
{
  wmOperatorType *ot = WM_operatortype_find("SEQUENCER_OT_sound_strip_add", true);
  if (ot == nullptr) {
    return;
  }

  PointerRNA ptr = WM_operator_properties_create_ptr(ot);
  RNA_string_set(&ptr, "filepath", state->filepath);
  RNA_int_set(&ptr, "frame_start", state->record_start_frame);
  RNA_int_set(&ptr, "channel", state->channel);
  RNA_boolean_set(&ptr, "replace_sel", false);
  RNA_boolean_set(&ptr, "overlap_shuffle_override", true);
  RNA_boolean_set(&ptr, "move_strips", false);
  WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &ptr, nullptr);
  WM_operator_properties_free(&ptr);
}

static const char *voiceover_input_device_name_get(const int input_device, ReportList *reports)
{
  static const char default_device_name[] = "";
  char **devices = BKE_sound_get_capture_device_names();
  if (devices == nullptr) {
    BKE_report(reports, RPT_WARNING, "Audio capture device backend is unavailable, using default");
    return default_device_name;
  }

  const int wanted_index = max_ii(0, input_device);
  for (int i = 0; devices[i] != nullptr; i++) {
    if (i == wanted_index) {
      return devices[i];
    }
  }

  if (devices[0] == nullptr) {
    BKE_report(reports, RPT_WARNING, "No audio capture devices were found, using default");
    return default_device_name;
  }

  BKE_reportf(reports,
              RPT_WARNING,
              "Voiceover input device index %d unavailable, using %s",
              input_device,
              devices[0]);
  return devices[0];
}

static void voiceover_stop_common(bContext *C, wmOperator *op, const bool cancel)
{
  VoiceoverState *state = static_cast<VoiceoverState *>(op->customdata);
  Scene *scene = CTX_data_sequencer_scene(C);
  wmWindowManager *wm = CTX_wm_manager(C);

  if (state->timer) {
    WM_event_timer_remove(wm, CTX_wm_window(C), state->timer);
    state->timer = nullptr;
  }

  if (state->session) {
    BKE_sound_voiceover_session_stop(state->session, cancel, op->reports);
    BKE_sound_voiceover_session_free(state->session);
    state->session = nullptr;
  }

  if (state->muted_during) {
    if (Scene *scene_eval = voiceover_scene_eval_get(C)) {
      BKE_sound_mute_scene(scene_eval, state->muted_before);
    }
    state->muted_during = false;
  }

  voiceover_feedback_strip_remove(scene, state);

  if (!state->was_playing && voiceover_is_playing_current_screen(C)) {
    ED_screen_animation_play(C, 0, 0);
  }

  if (scene->ed) {
    voiceover_countdown_set(scene->ed, 0);
    if (scene->ed->runtime != nullptr) {
      scene->ed->runtime->voiceover_recording = false;
    }
  }
}

static wmOperatorStatus sequencer_voiceover_record_exec(bContext *C, wmOperator *op)
{
  VoiceoverState *state = static_cast<VoiceoverState *>(op->customdata);
  voiceover_stop_common(C, op, false);

  if (state->started_recording) {
    voiceover_sound_strip_add(C, state);
  }

  MEM_delete(state);
  op->customdata = nullptr;

  Scene *scene = CTX_data_sequencer_scene(C);
  DEG_id_tag_update(&scene->id, ID_RECALC_SEQUENCER_STRIPS);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus sequencer_voiceover_record_cancel_exec(bContext *C, wmOperator *op)
{
  VoiceoverState *state = static_cast<VoiceoverState *>(op->customdata);
  voiceover_stop_common(C, op, true);

  MEM_delete(state);
  op->customdata = nullptr;
  return OPERATOR_CANCELLED;
}

static void sequencer_voiceover_record_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    sequencer_voiceover_record_cancel_exec(C, op);
  }
}

static wmOperatorStatus sequencer_voiceover_record_modal(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  VoiceoverState *state = static_cast<VoiceoverState *>(op->customdata);
  Scene *scene = CTX_data_sequencer_scene(C);

  if (event->type == EVT_ESCKEY) {
    return sequencer_voiceover_record_cancel_exec(C, op);
  }
  if (event->type == EVT_RETKEY) {
    return sequencer_voiceover_record_exec(C, op);
  }

  if (event->type != TIMER || event->customdata != state->timer) {
    return OPERATOR_PASS_THROUGH;
  }

  if (g_voiceover_stop_requested) {
    g_voiceover_stop_requested = false;
    return sequencer_voiceover_record_exec(C, op);
  }

  if (state->started_recording && !voiceover_is_playing_current_screen(C)) {
    return sequencer_voiceover_record_exec(C, op);
  }

  if (!state->started_recording) {
    Editing *ed = seq::editing_ensure(scene);
    if (state->pre_roll_seconds > 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now < state->pre_roll_deadline) {
        const double remaining_seconds =
            std::chrono::duration<double>(state->pre_roll_deadline - now).count();
        const int remaining = max_ii(1, int(std::ceil(remaining_seconds)));
        if (remaining != voiceover_countdown_get(ed)) {
          voiceover_countdown_set(ed, remaining);
        }
        WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, scene);
        return OPERATOR_PASS_THROUGH;
      }
    }

    voiceover_countdown_set(ed, 0);

    if (!voiceover_is_playing_current_screen(C)) {
      if (!voiceover_playback_start(C, op->reports)) {
        return sequencer_voiceover_record_cancel_exec(C, op);
      }
    }

    VoiceoverResolvedSettings resolved{};
    voiceover_resolve_settings(ed, &resolved);

    blender::SoundVoiceoverSettings settings{};
    settings.device_name = voiceover_input_device_name_get(resolved.input_device, op->reports);
    settings.filepath = state->filepath;
    settings.channels = resolved.audio_channels;
    settings.sample_rate = resolved.sample_rate;
    settings.sample_format = 0x24; /* float32 */
    settings.container = resolved.container;
    settings.codec = resolved.codec;
    settings.bitrate = resolved.bitrate;
    settings.gain = resolved.gain;

    state->session = BKE_sound_voiceover_session_start(&settings, op->reports);
    if (state->session == nullptr) {
      return sequencer_voiceover_record_cancel_exec(C, op);
    }

    state->record_start_frame = scene->r.cfra;
    if (!voiceover_feedback_strip_add(C, state)) {
      BKE_report(op->reports, RPT_ERROR, "Failed to create voiceover feedback strip");
      return sequencer_voiceover_record_cancel_exec(C, op);
    }

    state->started_recording = true;
    return OPERATOR_PASS_THROUGH;
  }

  BKE_sound_voiceover_session_update(state->session, op->reports);
  if (state->feedback_strip) {
    state->feedback_strip->right_handle_set(scene, scene->r.cfra + 1);
    state->channel = state->feedback_strip->channel;
    WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, scene);
  }

  return OPERATOR_PASS_THROUGH;
}

static wmOperatorStatus sequencer_voiceover_record_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent * /*event*/)
{
  if (!voiceover_poll(C)) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_sequencer_scene(C);
  if (scene->ed && scene->ed->runtime && scene->ed->runtime->voiceover_recording) {
    g_voiceover_stop_requested = true;
    return OPERATOR_FINISHED;
  }

  Editing *ed = seq::editing_ensure(scene);
  VoiceoverResolvedSettings resolved{};
  voiceover_resolve_settings(ed, &resolved);

  VoiceoverState *state = MEM_new<VoiceoverState>(__func__);
  state->initial_frame = scene->r.cfra;
  state->record_start_frame = scene->r.cfra;
  state->channel = resolved.channel;
  state->pre_roll_seconds = resolved.pre_roll;
  state->pre_roll_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(state->pre_roll_seconds);
  state->was_playing = voiceover_is_playing_current_screen(C);

  if (!voiceover_filepath_generate(C, resolved, state->filepath, op->reports)) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }

  if (resolved.mute_sound) {
    if (Scene *scene_eval = voiceover_scene_eval_get(C)) {
      state->muted_before = (scene_eval->audio.flag & AUDIO_MUTE) != 0;
      BKE_sound_mute_scene(scene_eval, true);
      state->muted_during = true;
    }
  }

  if (state->was_playing) {
    ED_screen_animation_play(C, 0, 0);
  }

  state->timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.02);
  scene->ed->runtime->voiceover_recording = true;
  voiceover_countdown_set(scene->ed, state->pre_roll_seconds);
  op->customdata = state;
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void SEQUENCER_OT_voiceover_record(wmOperatorType *ot)
{
  ot->name = "Record Voiceover";
  ot->description = "Record voiceover audio directly into the sequencer";
  ot->idname = "SEQUENCER_OT_voiceover_record";

  ot->invoke = sequencer_voiceover_record_invoke;
  ot->modal = sequencer_voiceover_record_modal;
  ot->exec = sequencer_voiceover_record_exec;
  ot->cancel = sequencer_voiceover_record_cancel;
  ot->poll = ED_operator_sequencer_active_editable;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender::ed::vse
