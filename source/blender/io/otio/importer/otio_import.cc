/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include "BKE_report.hh"

#include "BLI_listbase.hh"
#include "BLI_string.hh"

#include "CLG_log.h"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "IMB_imbuf_types.hh"

#include <opentime/rationalTime.h>
#include <opentime/timeRange.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/item.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>

#include "SEQ_add.hh"
#include "SEQ_utils.hh"

#include "IO_otio.hh"
#include "otio_import.hh"

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

static void add_clip(Main *bmain,
                     Scene *scene,
                     ListBaseT<Strip> *seqbase,
                     const Clip *clip,
                     int channel,
                     int left_handle,
                     bool is_sound_clip)
{
  TimeRange range = clip->trimmed_range();
  int left_offset = range.start_time().to_frames(scene->frames_per_second());
  int duration = range.duration().to_frames(scene->frames_per_second());

  seq::LoadData load_data;
  memset(&load_data, 0, sizeof(seq::LoadData));
  load_data.start_frame = left_handle - left_offset;
  load_data.channel = channel;
  load_data.allow_invalid_file = true;
  load_data.fit_method = SEQ_SCALE_TO_FIT;
  load_data.flags |= seq::SEQ_LOAD_SET_VIEW_TRANSFORM;
  load_data.flags &= ~seq::SEQ_LOAD_MOVIE_SYNC_FPS;
  load_data.image.count = 1;
  load_data.image.length = 1;
  STRNCPY(load_data.name, clip->name().c_str());

  StripType strip_type = is_sound_clip ? STRIP_TYPE_SOUND : STRIP_TYPE_MOVIE;

  if (auto ext_ref = dynamic_cast<ExternalReference *>(clip->media_reference())) {
    STRNCPY(load_data.path, ext_ref->target_url().c_str());

    if (BLI_path_extension_check_array(load_data.path, imb_ext_image)) {
      strip_type = STRIP_TYPE_IMAGE;
    }
  }
  else if (auto img_seq_ref = dynamic_cast<ImageSequenceReference *>(clip->media_reference())) {
    strip_type = STRIP_TYPE_IMAGE;
    /* Todo: Set load_data.image values.*/
  }

  Strip *strip = nullptr;

  switch (strip_type) {
    case STRIP_TYPE_MOVIE:
      strip = seq::add_movie_strip(bmain, scene, seqbase, &load_data);
      break;

    case STRIP_TYPE_SOUND:
      strip = seq::add_sound_strip(bmain, scene, seqbase, &load_data);
      break;

    case STRIP_TYPE_IMAGE:
      strip = seq::add_image_strip(bmain, scene, seqbase, &load_data);

      char dirpath[sizeof(strip->data->dirpath)];
      char filename[FILE_MAXFILE];
      BLI_path_split_dir_part(load_data.path, dirpath, sizeof(dirpath));
      BLI_path_split_file_part(load_data.path, filename, sizeof(filename));
      seq::add_image_set_directory(strip, dirpath);
      seq::add_image_load_file(scene, strip, 0, filename);
      seq::add_image_init_alpha_mode(bmain, scene, strip);
      break;

    default:
      break;
  }

  if (strip) {
    strip->handles_set(scene, left_handle, left_handle + duration + 1);
    if (!clip->enabled()) {
      strip->flag |= SEQ_MUTE;
    }
  }
  else {
    CLOG_ERROR(&LOG, "File '%s' could not be loaded", load_data.path);
  }
}

void build_blender_timeline(Main *bmain,
                            Scene *scene,
                            SerializableObject::Retainer<Timeline> &timeline,
                            ReportList *reports)
{
  /* Set Scene fps. */
  bool sync_fps = false;
  otio::ErrorStatus err;
  RationalTime dur = timeline->tracks()->duration(&err);
  if (is_error(err)) {
    CLOG_WARN(&LOG,
              "Failed to determine timeline FPS\nDetails: %s\nDescription: %s",
              err.details.c_str(),
              err.full_description.c_str());
    BKE_reportf(reports, RPT_WARNING, "OTIO Import: Failed to determine timeline FPS");
    sync_fps = true;
  }
  else {
    const double rate = dur.rate();
    scene->r.frs_sec = static_cast<short>(rate);
    scene->r.frs_sec_base = static_cast<float>(scene->r.frs_sec / rate);
  }
  int channel = 1;
  std::vector<Track *> audio_tracks = timeline->audio_tracks();
  std::vector<Track *> video_tracks = timeline->video_tracks();

  for (Track *track : audio_tracks) {
    int left_handle = 1;
    for (auto child : track->children()) {
      if (auto item = dynamic_cast<Item *>(child.value)) {
        if (auto clip = dynamic_cast<Clip *>(item)) {
          add_clip(bmain, scene, &scene->ed->seqbase, clip, channel, left_handle, true);
        }
        else if (dynamic_cast<Gap *>(item)) {
          /* Pass. */
        }
        left_handle += item->trimmed_range().duration().to_frames(scene->frames_per_second()) + 1;
      }
    }
    ++channel;
  }

  for (Track *track : video_tracks) {
    int left_handle = 1;
    for (auto child : track->children()) {
      if (auto item = dynamic_cast<Item *>(child.value)) {
        if (auto clip = dynamic_cast<Clip *>(item)) {
          add_clip(bmain, scene, &scene->ed->seqbase, clip, channel, left_handle, false);
        }
        else if (dynamic_cast<Gap *>(item)) {
          /* Pass. */
        }
        left_handle += item->trimmed_range().duration().to_frames(scene->frames_per_second()) + 1;
      }
    }
    ++channel;
  }
}

}  // namespace blender::io::otio
