/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cstring>

#include "BLI_math_base.h"
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
  int space_between = _strip->left_handle() - last_strip_end - 1;
  if (space_between > 0) {
    auto gap_duration = RationalTime(space_between, _scene->frames_per_second());
    auto gap = SerializableObject::Retainer<Gap>(new Gap(gap_duration));
    _track->append_child(gap);
  }
  last_strip_end = _strip->right_handle(_scene);
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

static int get_strip_duration(const Strip *strip, const Scene *scene)
{
  return strip->right_handle(scene) - strip->left_handle();
}

static TimeRange get_media_available_range(const Strip *strip,
                                           const Scene *scene,
                                           const float media_fps)
{
  int media_length = (strip->type == STRIP_TYPE_IMAGE) ? get_strip_duration(strip, scene) :
                                                         strip->len;
  return TimeRange(otio::RationalTime(0, media_fps), otio::RationalTime(media_length, media_fps));
}

static TimeRange get_strip_source_range(const Strip *strip,
                                        const Scene *scene,
                                        const float media_fps)
{
  /* The strips could be moved left of the timeline start and could have -ve `strip->start`. Most
   * NLE's doesn't have the concept of moving a strip left of the timeline. */
  int left_offset = max_ii(0, -strip->start);

  return TimeRange(RationalTime(strip->startofs + left_offset, media_fps),
                   RationalTime(get_strip_duration(strip, scene) - left_offset, media_fps));
}

static SerializableObject::Retainer<ExternalReference> create_external_reference(
    const Strip *strip, const Scene *scene, float media_fps)
{
  char media_filename[FILE_MAX];
  char media_filepath[FILE_MAX];
  get_media_filename(strip, media_filename);
  get_media_filepath(strip, media_filepath);

  TimeRange media_available_range = get_media_available_range(strip, scene, media_fps);

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

  TimeRange strip_source_range = get_strip_source_range(_strip, _scene, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      _strip, _scene, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(_strip->name, external_reference, strip_source_range));

  _track->append_child(clip);
}

void SoundStripExporter::export_strip()
{
  add_gap_if_necessary();

  float media_fps = _scene->frames_per_second();

  TimeRange strip_source_range = get_strip_source_range(_strip, _scene, media_fps);
  SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
      _strip, _scene, media_fps);

  auto clip = otio::SerializableObject::Retainer<otio::Clip>(
      new Clip(_strip->name, external_reference, strip_source_range));

  _track->append_child(clip);
}

void ImageStripExporter::export_strip()
{
  add_gap_if_necessary();

  bool is_single_image = _strip->flag & SEQ_SINGLE_FRAME_CONTENT;
  float media_fps = _scene->frames_per_second();

  if (is_single_image) {
    TimeRange strip_source_range = get_strip_source_range(_strip, _scene, media_fps);
    SerializableObject::Retainer<ExternalReference> external_reference = create_external_reference(
        _strip, _scene, media_fps);

    auto clip = otio::SerializableObject::Retainer<otio::Clip>(
        new Clip(_strip->name, external_reference, strip_source_range));

    _track->append_child(clip);
  }
}

}  // namespace blender::io::otio
