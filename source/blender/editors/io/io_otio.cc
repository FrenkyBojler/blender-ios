/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#ifdef WITH_OTIO

#  include <cerrno>
#  include <cstring>
#  include <map>

#  include "DNA_modifier_types.h"
#  include "DNA_object_types.h"
#  include "DNA_scene_types.h"
#  include "DNA_space_enums.h"

#  include "BKE_context.hh"
#  include "BKE_file_handler.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"

#  include "BLI_listbase_iterator.hh"
#  include "BLI_path_utils.hh"
#  include "BLI_string_utf8.h"
#  include "BLI_utildefines.h"
#  include "BLI_vector.hh"

#  include "BLT_translation.hh"

#  include "RNA_access.hh"
#  include "RNA_define.hh"
#  include "RNA_enum_types.hh"

#  include "ED_fileselect.hh"
#  include "ED_object.hh"

#  include "UI_interface.hh"
#  include "UI_resources.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"

#  include "DEG_depsgraph.hh"

#  include "SEQ_sequencer.hh"

#  include "io_otio.hh"
#  include "io_utils.hh"
#  include "opentime/rationalTime.h"
#  include "opentime/timeRange.h"
#  include "opentimelineio/clip.h"
#  include "opentimelineio/externalReference.h"
#  include "opentimelineio/gap.h"
#  include "opentimelineio/timeline.h"
#  include "opentimelineio/track.h"

#  include "UI_interface_layout.hh"

#  include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"io.otio"};

enum scene_strip_resolution {
  SCENE_STRIP_25_PERCENT,
  SCENE_STRIP_50_PERCENT,
  SCENE_STRIP_75_PERCENT,
  SCENE_STRIP_100_PERCENT,
};

static const EnumPropertyItem io_otio_scene_strip_resolution[] = {
    {SCENE_STRIP_100_PERCENT,
     "SCENE_STRIP_100",
     ICON_NONE,
     "100%",
     "Bake Scene Strips at 100% Resolution"},
    {SCENE_STRIP_75_PERCENT,
     "SCENE_STRIP_75",
     ICON_NONE,
     "75%",
     "Bake Scene Strips at 75% Resolution"},
    {SCENE_STRIP_50_PERCENT,
     "SCENE_STRIP_50",
     ICON_NONE,
     "50%",
     "Bake Scene Strips at 50% Resolution"},
    {SCENE_STRIP_25_PERCENT,
     "SCENE_STRIP_25",
     ICON_NONE,
     "25%",
     "Bake Scene Strips at 25% Resolution"},
    {0, nullptr, 0, nullptr, nullptr}};

static wmOperatorStatus wm_otio_export_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  if (!RNA_struct_property_is_set(op->ptr, "as_background_job")) {
    RNA_boolean_set(op->ptr, "as_background_job", true);
  }

  ED_fileselect_ensure_default_filepath(C, op, ".otio");

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus wm_otio_export_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  Scene *scene = CTX_data_sequencer_scene(C);
  Editing *editing = seq::editing_get(scene);
  ListBaseT<Strip> *seqbase = &editing->seqbase;

  if (!scene || !editing) {
    BKE_report(op->reports, RPT_ERROR, "No Sequencer Scene found");
    return OPERATOR_CANCELLED;
  }

  namespace otio = opentimelineio::OPENTIMELINEIO_VERSION_NS;

  auto timeline = otio::SerializableObject::Retainer<otio::Timeline>(
      new otio::Timeline(scene->id.name));
  auto main_stack = otio::SerializableObject::Retainer<otio::Stack>(new otio::Stack());

  auto compare_strip_channels = [](const Strip *a, const Strip *b) { return a->start < b->start; };
  std::map<int, std::set<Strip *, decltype(compare_strip_channels)>> video_channels;
  std::map<int, std::set<Strip *, decltype(compare_strip_channels)>> audio_channels;

  for (Strip &strip : *seqbase) {
    if (ELEM(strip.type, STRIP_TYPE_SOUND)) {
      audio_channels[strip.channel].insert(&strip);
    }
    else {
      video_channels[strip.channel].insert(&strip);
    }
  }
  /* First Add Video Tracks in the Stack. */
  for (auto [original_channel, strips] : video_channels) {

    auto track_source_range = otio::TimeRange(
        otio::RationalTime(0, scene->frames_per_second()),
        otio::RationalTime(scene->r.efra, scene->frames_per_second()));

    auto track = otio::SerializableObject::Retainer<otio::Track>(
        new otio::Track("", track_source_range, otio::Track::Kind::video));

    int last_strip_end = 0;
    /* Append all the strips of this channel in the track. */
    for (Strip *strip : strips) {
      /* Only export Movie strips for now. */
      if (!ELEM(strip->type, STRIP_TYPE_MOVIE)) {
        continue;
      }
      int space_between = strip->start - last_strip_end;
      if (space_between > 0) {
        /* Add gap object. */
        auto gap_duration = otio::RationalTime(space_between, scene->frames_per_second());
        auto gap = otio::SerializableObject::Retainer<otio::Gap>(new otio::Gap(gap_duration));
        track->append_child(gap);
      }
      char media_filepath[FILE_MAX];
      BLI_path_join(media_filepath,
                    sizeof(media_filepath),
                    strip->data->dirpath,
                    strip->data->stripdata->filename);

      auto media_available_range = otio::TimeRange(
          otio::RationalTime(0, strip->media_fps(scene)),
          otio::RationalTime(strip->len, strip->media_fps(scene)));

      auto strip_source_range = otio::TimeRange(
          otio::RationalTime(strip->startofs, strip->media_fps(scene)),
          otio::RationalTime(strip->len - (strip->startofs + strip->endofs),
                             strip->media_fps(scene)));

      auto external_reference = otio::SerializableObject::Retainer<otio::ExternalReference>(
          new otio::ExternalReference(media_filepath, media_available_range));

      external_reference->set_name(strip->data->stripdata->filename);

      auto clip = otio::SerializableObject::Retainer<otio::Clip>(
          new otio::Clip(strip->name, external_reference, strip_source_range));

      track->append_child(clip);

      last_strip_end = strip->start + (strip->len - (strip->startofs + strip->endofs));
    }
    if (scene->r.efra - last_strip_end > 0) {
      auto gap_duration = otio::RationalTime(scene->r.efra - last_strip_end,
                                             scene->frames_per_second());
      auto gap = otio::SerializableObject::Retainer<otio::Gap>(new otio::Gap(gap_duration));
      track->append_child(gap);
    }

    main_stack->append_child(track);
  }

  timeline->set_tracks(main_stack);
  timeline->to_json_file(filepath);

  BKE_report(op->reports, RPT_INFO, "File exported successfully");
  return OPERATOR_FINISHED;
}

static void ui_otio_export_settings(const bContext *C, ui::Layout &layout, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  /* Scene Strip Options */
  if (ui::Layout *panel = layout.panel(C, "OTIO_export_scene", false, IFACE_("Scene Strips"))) {
    ui::Layout *col = &panel->column(false);
    col->prop(ptr, "bake_scene", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    ui::Layout *sub = &col->column(false);
    sub->enabled_set(RNA_boolean_get(ptr, "bake_scene"));
    sub->prop(ptr, "scene_strip_resolution", UI_ITEM_NONE, IFACE_("Resolution"), ICON_NONE);
  }
}

static void wm_otio_export_draw(bContext *C, wmOperator *op)
{
  ui_otio_export_settings(C, *op->layout, op->ptr);
}

static bool wm_otio_export_check(bContext * /*C*/, wmOperator *op)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  if (!BLI_path_extension_check(filepath, ".otio")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".otio");
    RNA_string_set(op->ptr, "filepath", filepath);
    return true;
  }

  return false;
}

void WM_OT_otio_export(wmOperatorType *ot)
{
  ot->name = "Export OTIO";
  ot->description = "Export Sequencer Scene as OpenTimelineIO Timeline";
  ot->idname = "WM_OT_otio_export";

  ot->invoke = wm_otio_export_invoke;
  ot->exec = wm_otio_export_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_otio_export_draw;
  ot->check = wm_otio_export_check;
  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  PropertyRNA *prop = RNA_def_string(ot->srna, "filter_glob", "*.otio", 0, "", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_boolean(
      ot->srna, "bake_scene", true, "Bake Scene Strips", "Export Scene Strips as Video");

  RNA_def_enum(ot->srna,
               "scene_strip_resolution",
               io_otio_scene_strip_resolution,
               SCENE_STRIP_100_PERCENT,
               "Scene Strip Resolution",
               "Resolution at which to Export the Scene Strips");

  RNA_def_boolean(
      ot->srna,
      "as_background_job",
      false,
      "Run as Background Job",
      "Enable this to run the import in the background, disable to block Blender while importing. "
      "This option is deprecated; EXECUTE this operator to run in the foreground, and INVOKE it "
      "to run as a background job");
}

namespace ed::io {
void otio_file_handler_add()
{
  auto fh = std::make_unique<bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_otio");
  STRNCPY_UTF8(fh->export_operator, "WM_OT_otio_export");
  STRNCPY_UTF8(fh->label, "OpenTimelineIO");
  STRNCPY_UTF8(fh->file_extensions_str, ".otio");
  fh->poll_drop = poll_file_object_drop;
  bke::file_handler_add(std::move(fh));
}
}  // namespace ed::io
}  // namespace blender

#endif
