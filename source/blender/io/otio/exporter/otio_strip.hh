/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "BLI_path_utils.hh"

#include "opentimelineio/serializableObject.h"
#include "opentimelineio/track.h"

namespace blender {
struct Strip;
struct Scene;

namespace io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

/* Inherit this class and define the `export_strip()` member function to export a new strip type.
 */
class StripExporter {
 public:
  int last_strip_end = 0;

  StripExporter(Strip *strip,
                Scene *scene,
                SerializableObject::Retainer<Track> &track,
                int last_strip_end = 0)
      : last_strip_end(last_strip_end), _strip(strip), _scene(scene), _track(track) {};

  virtual ~StripExporter() {};

  virtual void export_strip() = 0;

  void add_gap_if_necessary();
  static void add_gap_if_necessary(SerializableObject::Retainer<Track> &_track,
                                   int left_frame,
                                   int right_frame,
                                   double scene_fps);

 protected:
  Strip *_strip;
  Scene *_scene;
  SerializableObject::Retainer<Track> &_track;
};

class MovieStripExporter : public StripExporter {
 public:
  MovieStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0)
      : StripExporter(strip, scene, track, last_strip_end) {};

  void export_strip() override;
};

class SoundStripExporter : public StripExporter {
 public:
  SoundStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0)
      : StripExporter(strip, scene, track, last_strip_end) {};

  void export_strip() override;
};

class ImageStripExporter : public StripExporter {
 public:
  ImageStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0)
      : StripExporter(strip, scene, track, last_strip_end) {};

  void export_strip() override;
};

}  // namespace io::otio
}  // namespace blender
