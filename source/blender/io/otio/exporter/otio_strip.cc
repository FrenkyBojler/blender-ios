/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cstring>

#include "BLI_path_utils.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/track.h"

#include "otio_strip.hh"

namespace blender::io::otio {

using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

void StripExporter::add_gap_if_necessary()
{
  int space_between = _strip->start - last_strip_end - 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, _scene->frames_per_second());
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    _track->append_child(gap);
  }
  last_strip_end = _strip->start + (_strip->len - (_strip->startofs + _strip->endofs));
}

void StripExporter::add_gap_if_necessary(SerializableObject::Retainer<Track> &track,
                                         int left_frame,
                                         int right_frame,
                                         double scene_fps)
{
  int space_between = right_frame - left_frame - 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, scene_fps);
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    track->append_child(gap);
  }
}

/************** Helper Functions. **************/

static void get_media_filename(const Strip *strip, char *filename_out)
{
  strcpy(filename_out, strip->data->stripdata->filename);
}

static void get_media_filepath(const Strip *strip, char *filepath_out)
{
  BLI_path_join(filepath_out, FILE_MAX, strip->data->dirpath, strip->data->stripdata->filename);
}

static TimeRange get_media_available_range(const Strip *strip, const float media_fps)
{
  return TimeRange(otio::RationalTime(0, media_fps), otio::RationalTime(strip->len, media_fps));
}

static TimeRange get_strip_source_range(const Strip *strip, const float media_fps)
{
  int strip_duration_frames = strip->len - (strip->startofs + strip->endofs);
  return TimeRange(RationalTime(strip->startofs, media_fps),
                   RationalTime(strip_duration_frames, media_fps));
}

static SerializableObject::Retainer<ExternalReference> create_external_reference(Strip *strip,
                                                                                 float media_fps)
{
  char media_filename[FILE_MAX];
  char media_filepath[FILE_MAX];
  get_media_filename(strip, media_filename);
  get_media_filepath(strip, media_filepath);

  TimeRange media_available_range = get_media_available_range(strip, media_fps);

  auto external_reference = SerializableObject::Retainer<ExternalReference>(
      new ExternalReference(media_filepath, media_available_range));

  external_reference->set_name(media_filename);

  return external_reference;
}

/***** Handle Export for each strip type. *****/

void MovieStripExporter::export_strip()
{
  add_gap_if_necessary();

  float media_fps = _scene->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(_strip, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      _strip, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(_strip->name, external_reference, strip_source_range));

  _track->append_child(clip);
}

void SoundStripExporter::export_strip()
{
  add_gap_if_necessary();

  float media_fps = _scene->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(_strip, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      _strip, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(_strip->name, external_reference, strip_source_range));

  _track->append_child(clip);
}

}  // namespace blender::io::otio
