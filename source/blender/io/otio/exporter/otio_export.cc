/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <map>

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_sequencer.hh"

#include "opentime/rationalTime.h"
#include "opentime/timeRange.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"

#include "IO_otio.hh"
#include "otio_export.hh"
#include "otio_strip.hh"

namespace blender {
namespace io::otio {

using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

wmOperatorStatus otio_export_exec(bContext *C, const blender::OTIOExportParams *export_params)
{

  Scene *scene = CTX_data_sequencer_scene(C);
  Editing *editing = seq::editing_get(scene);
  ListBaseT<Strip> *seqbase = &editing->seqbase;

  if (!scene || !editing) {
    BKE_report(export_params->reports, RPT_ERROR, "No Sequencer Scene found");
    return OPERATOR_CANCELLED;
  }

  auto timeline = SerializableObject::Retainer<Timeline>(new Timeline(scene->id.name));
  auto main_stack = SerializableObject::Retainer<Stack>(new Stack());

  /* Separate video and audio channels. */
  auto compare_strip_start = [](const Strip *a, const Strip *b) { return a->start < b->start; };
  std::map<int, std::set<Strip *, decltype(compare_strip_start)>> video_channels;
  std::map<int, std::set<Strip *, decltype(compare_strip_start)>> audio_channels;

  for (Strip &strip : *seqbase) {
    if (ELEM(strip.type, STRIP_TYPE_SOUND)) {
      audio_channels[strip.channel].insert(&strip);
    }
    else {
      video_channels[strip.channel].insert(&strip);
    }
  }

  /* First add video (visual) tracks in the stack. */
  for (auto [original_channel, strips] : video_channels) {

    auto track_source_range = otio::TimeRange(
        RationalTime(0, scene->frames_per_second()),
        RationalTime(scene->r.efra, scene->frames_per_second()));

    auto track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, Track::Kind::video));

    int last_strip_end = 0;
    /* Append all the strips of this channel in the track. */
    for (Strip *strip : strips) {
      StripExporter *strip_exporter = nullptr;

      switch (strip->type) {
        case STRIP_TYPE_MOVIE:
          strip_exporter = new MovieStripExporter(strip, scene, track, last_strip_end);
          break;

        default:
          break;
      }

      if (strip_exporter) {
        strip_exporter->export_strip();
        last_strip_end = strip_exporter->last_strip_end;

        delete strip_exporter;
      }
    }
    StripExporter::add_gap_if_necessary(
        track, last_strip_end, scene->r.efra, scene->frames_per_second());

    main_stack->append_child(track);
  }

  /* Now add audio tracks in the stack. */
  for (auto [original_channel, strips] : audio_channels) {

    auto track_source_range = otio::TimeRange(
        RationalTime(0, scene->frames_per_second()),
        RationalTime(scene->r.efra, scene->frames_per_second()));

    auto track = SerializableObject::Retainer<Track>(
        new Track("", track_source_range, Track::Kind::audio));

    int last_strip_end = 0;
    /* Append all the strips of this channel in the track. */
    for (Strip *strip : strips) {
      if (strip->type != STRIP_TYPE_SOUND) {
        continue;
      }

      SoundStripExporter *sound_strip_exporter = new SoundStripExporter(
          strip, scene, track, last_strip_end);

      sound_strip_exporter->export_strip();
      last_strip_end = sound_strip_exporter->last_strip_end;

      delete sound_strip_exporter;
    }
    StripExporter::add_gap_if_necessary(
        track, last_strip_end, scene->r.efra, scene->frames_per_second());

    main_stack->append_child(track);
  }

  timeline->set_tracks(main_stack);
  timeline->to_json_file(export_params->filepath);

  return OPERATOR_FINISHED;
}

}  // namespace io::otio
}  // namespace blender
