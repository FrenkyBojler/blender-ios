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
struct Main;
struct OTIOExportParams;

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
                int last_strip_end = 0,
                const char *filepath = nullptr)
      : last_strip_end(last_strip_end),
        _strip(strip),
        _scene(scene),
        _track(track),
        _filepath(filepath) {};

  virtual ~StripExporter() {};

  virtual void export_strip(Main * /*bmain*/, const OTIOExportParams * /*export_params*/) {};

  void add_gap_if_necessary();
  static void add_gap_if_necessary(SerializableObject::Retainer<Track> &_track,
                                   int start_frame,
                                   int end_frame,
                                   double scene_fps);

  void export_with_missing_reference();

 protected:
  Strip *_strip;
  Scene *_scene;
  SerializableObject::Retainer<Track> &_track;
  const char *_filepath;
};

class MovieStripExporter : public StripExporter {
 public:
  MovieStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0,
                     const char *filepath = nullptr)
      : StripExporter(strip, scene, track, last_strip_end, filepath) {};

  void export_strip(Main *bmain, const OTIOExportParams *export_params) override;
};

class SoundStripExporter : public StripExporter {
 public:
  SoundStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0)
      : StripExporter(strip, scene, track, last_strip_end) {};

  void export_strip(Main *bmain, const OTIOExportParams *export_params) override;
};

class ImageStripExporter : public StripExporter {
 public:
  ImageStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0,
                     const char *filepath = nullptr)
      : StripExporter(strip, scene, track, last_strip_end, filepath) {};

  void export_strip(Main *bmain, const OTIOExportParams *export_params) override;
};

class RenderAsMovieExporter : public StripExporter {
 public:
  RenderAsMovieExporter(Strip *strip,
                        Scene *scene,
                        SerializableObject::Retainer<Track> &track,
                        int last_strip_end = 0,
                        const char *filepath = nullptr,
                        const bool include_audio = false)
      : StripExporter(strip, scene, track, last_strip_end, filepath),
        _include_audio(include_audio) {};

  void export_strip(Main *bmain, const OTIOExportParams *export_params) override;

 private:
  bool _include_audio = false;
};

}  // namespace io::otio
}  // namespace blender
