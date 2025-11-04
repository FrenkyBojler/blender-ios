// Copyright (c) 2014 libmv authors.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// Author: mierle@gmail.com (Keir Mierle)

#include "libmv/autotrack/autotrack.h"
#include "libmv/autotrack/frame_accessor.h"
#include "libmv/autotrack/predict_tracks.h"
#include "libmv/autotrack/quad.h"
#include "libmv/base/scoped_ptr.h"
#include "libmv/logging/logging.h"
#include "libmv/numeric/numeric.h"
#include "libmv/simple_pipeline/detect.h"

#include <cfloat>

namespace mv {

namespace {

class DisableChannelsTransform : public FrameAccessor::Transform {
 public:
  DisableChannelsTransform(int disabled_channels)
      : disabled_channels_(disabled_channels) {}

  int64_t key() const { return disabled_channels_; }

  void run(const FloatImage& input, FloatImage* output) const {
    bool disable_red = (disabled_channels_ & Marker::CHANNEL_R) != 0,
         disable_green = (disabled_channels_ & Marker::CHANNEL_G) != 0,
         disable_blue = (disabled_channels_ & Marker::CHANNEL_B) != 0;

    LG << "Disabling channels: " << (disable_red ? "R " : "")
       << (disable_green ? "G " : "") << (disable_blue ? "B" : "");

    // It's important to rescale the resultappropriately so that e.g. if only
    // blue is selected, it's not zeroed out.
    float scale = (disable_red ? 0.0f : 0.2126f) +
                  (disable_green ? 0.0f : 0.7152f) +
                  (disable_blue ? 0.0f : 0.0722f);

    output->Resize(input.Height(), input.Width(), 1);
    for (int y = 0; y < input.Height(); y++) {
      for (int x = 0; x < input.Width(); x++) {
        float r = disable_red ? 0.0f : input(y, x, 0);
        float g = disable_green ? 0.0f : input(y, x, 1);
        float b = disable_blue ? 0.0f : input(y, x, 2);
        (*output)(y, x, 0) = (0.2126f * r + 0.7152f * g + 0.0722f * b) / scale;
      }
    }
  }

 private:
  // Bitfield representing visible channels, bits are from Marker::Channel.
  int disabled_channels_;
};

template <typename QuadT, typename ArrayT>
void QuadToArrays(const QuadT& quad, ArrayT* x, ArrayT* y) {
  for (int i = 0; i < 4; ++i) {
    x[i] = quad.coordinates(i, 0);
    y[i] = quad.coordinates(i, 1);
  }
}

void MarkerToArrays(const Marker& marker, double* x, double* y) {
  Quad2Df offset_quad = marker.patch;
  Vec2f origin = marker.search_region.Rounded().min;
  offset_quad.coordinates.rowwise() -= origin.transpose();
  QuadToArrays(offset_quad, x, y);
  x[4] = marker.center.x() - origin(0);
  y[4] = marker.center.y() - origin(1);
}

FrameAccessor::Key GetImageForMarker(const Marker& marker,
                                     FrameAccessor* frame_accessor,
                                     FloatImage* image) {
  // TODO(sergey): Currently we pass float region to the accessor,
  // but we don't want the accessor to decide the rounding, so we
  // do rounding here.
  // Ideally we would need to pass IntRegion to the frame accessor.
  Region region = marker.search_region.Rounded();
  libmv::scoped_ptr<FrameAccessor::Transform> transform = NULL;
  if (marker.disabled_channels != 0) {
    transform.reset(new DisableChannelsTransform(marker.disabled_channels));
  }
  return frame_accessor->GetImage(marker.clip,
                                  marker.frame,
                                  FrameAccessor::MONO,
                                  0,  // No downscale for now.
                                  &region,
                                  transform.get(),
                                  image);
}

FrameAccessor::Key GetMaskForMarker(const Marker& marker,
                                    FrameAccessor* frame_accessor,
                                    FloatImage* mask) {
  Region region = marker.search_region.Rounded();
  return frame_accessor->GetMaskForTrack(
      marker.clip, marker.frame, marker.track, &region, mask);
}

PredictDirection getPredictDirection(const TrackRegionOptions* track_options) {
  switch (track_options->direction) {
    case TrackRegionOptions::FORWARD: return PredictDirection::FORWARD;
    case TrackRegionOptions::BACKWARD: return PredictDirection::BACKWARD;
  }

  LOG(FATAL) << "Unhandled tracking direction " << track_options->direction
             << ", should never happen.";

  return PredictDirection::AUTO;
}

}  // namespace

bool AutoTrack::TrackMarker(Marker* tracked_marker,
                            TrackRegionResult* result,
                            const TrackRegionOptions* track_options) {
  // Try to predict the location of the second marker.
  const PredictDirection predict_direction = getPredictDirection(track_options);
  bool predicted_position = false;
  if (PredictMarkerPosition(tracks_, predict_direction, tracked_marker)) {
    LG << "Successfully predicted!";
    predicted_position = true;
  } else {
    LG << "Prediction failed; trying to track anyway.";
  }

  Marker reference_marker;
  tracks_.GetMarker(tracked_marker->reference_clip,
                    tracked_marker->reference_frame,
                    tracked_marker->track,
                    &reference_marker);

  // Convert markers into the format expected by TrackRegion.
  double x1[5], y1[5];
  MarkerToArrays(reference_marker, x1, y1);

  double x2[5], y2[5];
  MarkerToArrays(*tracked_marker, x2, y2);

  // TODO(keir): Technically this could take a smaller slice from the source
  // image instead of taking one the size of the search window.
  FloatImage reference_image;
  FrameAccessor::Key reference_key =
      GetImageForMarker(reference_marker, frame_accessor_, &reference_image);
  if (!reference_key) {
    LG << "Couldn't get frame for reference marker: " << reference_marker;
    return false;
  }

  FloatImage reference_mask;
  FrameAccessor::Key reference_mask_key =
      GetMaskForMarker(reference_marker, frame_accessor_, &reference_mask);

  FloatImage tracked_image;
  FrameAccessor::Key tracked_key =
      GetImageForMarker(*tracked_marker, frame_accessor_, &tracked_image);
  if (!tracked_key) {
    frame_accessor_->ReleaseImage(reference_key);
    LG << "Couldn't get frame for tracked marker: " << tracked_marker;
    return false;
  }

  // Store original position before tracking, so we can claculate offset later.
  Vec2f original_center = tracked_marker->center;

  // Do the tracking!
  TrackRegionOptions local_track_region_options;
  if (track_options) {
    local_track_region_options = *track_options;
  }
  if (reference_mask_key != NULL) {
    LG << "Using mask for reference marker: " << reference_marker;
    local_track_region_options.image1_mask = &reference_mask;
  }
  local_track_region_options.num_extra_points = 1;  // For center point.
  local_track_region_options.attempt_refine_before_brute = predicted_position;
  TrackRegion(reference_image,
              tracked_image,
              x1,
              y1,
              local_track_region_options,
              x2,
              y2,
              result);

  // Copy results over the tracked marker.
  Vec2f tracked_origin = tracked_marker->search_region.Rounded().min;
  for (int i = 0; i < 4; ++i) {
    tracked_marker->patch.coordinates(i, 0) = x2[i] + tracked_origin[0];
    tracked_marker->patch.coordinates(i, 1) = y2[i] + tracked_origin[1];
  }
  tracked_marker->center(0) = x2[4] + tracked_origin[0];
  tracked_marker->center(1) = y2[4] + tracked_origin[1];
  Vec2f delta = tracked_marker->center - original_center;
  tracked_marker->search_region.Offset(delta);
  tracked_marker->source = Marker::TRACKED;
  tracked_marker->status = Marker::UNKNOWN;
  tracked_marker->reference_clip = reference_marker.clip;
  tracked_marker->reference_frame = reference_marker.frame;

  // Release the images and masks from the accessor cache.
  frame_accessor_->ReleaseImage(reference_key);
  frame_accessor_->ReleaseImage(tracked_key);
  frame_accessor_->ReleaseMask(reference_mask_key);

  // TODO(keir): Possibly the return here should get removed since the results
  // are part of TrackResult. However, eventually the autotrack stuff will have
  // extra status (e.g. prediction fail, etc) that should get included.
  return true;
}

void AutoTrack::AddMarker(const Marker& marker) {
  tracks_.AddMarker(marker);
}

void AutoTrack::SetMarkers(vector<Marker>* markers) {
  tracks_.SetMarkers(markers);
}

bool AutoTrack::GetMarker(int clip,
                          int frame,
                          int track,
                          Marker* markers) const {
  return tracks_.GetMarker(clip, frame, track, markers);
}

void AutoTrack::GetMarkersInFrame(int clip, int frame, libmv::vector<mv::Marker>* markers) {
  return tracks_.GetMarkersInFrame(clip, frame, markers);
}

void AutoTrack::DetectFeaturesInFrame(int clip, int frame, const libmv::DetectOptions& options) {
  vector<libmv::Feature> detected_features;

  // FIXME: if we don't pass a region, the [image data?] is somehow corrupted and all (current and future) tracking operations fail
  libmv::FloatImage float_image;
  int width, height;
  frame_accessor_->GetClipDimensions(clip, frame, &width, &height);
  Region region;
  region.min = Vec2f(0, 0);
  region.max = Vec2f(static_cast<float>(width), static_cast<float>(height));
  FrameAccessor::Key frame_key = frame_accessor_->GetImage(clip, frame, mv::FrameAccessor::InputMode::RGBA, 0, &region, nullptr, &float_image);

  if (!frame_key) {
    return;
  }

  libmv::Detect(float_image, options, &detected_features);

  frame_accessor_->ReleaseImage(frame_key);

  for (libmv::Feature feature : detected_features) {
    mv::Marker marker;
    marker.clip = clip;
    marker.frame = frame;
    marker.reference_clip = clip;
    marker.reference_frame = frame;

    if (tracks_.markers().size() == 0) {
      marker.track = 0;
    } else {
      marker.track = tracks_.MaxTrack() + 1;
    }

    marker.status = Marker::Status::UNKNOWN;
    marker.source = Marker::Source::TRACKED;
    marker.model_type = Marker::ModelType::POINT;
    marker.model_id = 0;
    marker.disabled_channels = 0;

    marker.center[0] = feature.x;
    marker.center[1] = feature.y;

    float pattern_size = 21.0f;
    float search_size = 71.0f;

    float half_pattern_size = pattern_size / 2.0f;
    marker.patch.coordinates <<
      feature.x - half_pattern_size, feature.y - half_pattern_size,
      feature.x + half_pattern_size, feature.y - half_pattern_size,
      feature.x + half_pattern_size, feature.y + half_pattern_size,
      feature.x - half_pattern_size, feature.y + half_pattern_size;

    float search_margin = search_size / 2.0f;
    marker.search_region.min = Vec2f(feature.x - search_margin,
                                    feature.y - search_margin);
    marker.search_region.max = Vec2f(feature.x + search_margin,
                                    feature.y + search_margin);

    AddMarker(marker);
    // TODO: we need a way to sync the new track so frame accessor can do masking.
  }
}

libmv::vector<mv::Marker> AutoTrack::Markers() {
  return tracks_.markers();
}

void AutoTrack::DetectAndTrack(const DetectAndTrackOptions& options) {
  int num_clips = frame_accessor_->NumClips();
  for (int clip = 0; clip < num_clips; ++clip) {
    int num_frames = frame_accessor_->NumFrames(clip);
    if (num_frames < 2)
      continue; // nothing to track
    vector<Marker> this_frame_markers;
    for (int frame = 1; frame < num_frames; ++frame) {
      if (!options.step_callback(options.user_data, frame) || Cancelled()) {
        LG << "Got cancel message while detecting and tracking...";
        return;
      }
      // detect initial features if there aren't enough
      this_frame_markers.clear();
      GetMarkersInFrame(clip, frame, &this_frame_markers);
      if (this_frame_markers.size() < options.min_num_features) {
        DetectFeaturesInFrame(clip, frame, options.detect_options);
        this_frame_markers.clear();
        GetMarkersInFrame(clip, frame, &this_frame_markers);
      }

      int width, height;
      frame_accessor_->GetClipDimensions(clip, frame, &width, &height);
      float frame_width = static_cast<float>(width);
      float frame_height = static_cast<float>(height);

      // track features forward
      for (Marker marker : this_frame_markers) {
        marker.reference_frame = frame;
        marker.frame = frame + 1;
        TrackRegionResult result;
        TrackMarker(&marker, &result, &this->options.track_region);

        if (result.is_usable()) {
          // check if the marker is still in bounds.
          // this prevents markers sliding along the edge of the frame.
          Vec2f patch_min = marker.patch.coordinates.colwise().minCoeff();
          Vec2f patch_max = marker.patch.coordinates.colwise().maxCoeff();

          float margin = static_cast<float>(this->options.track_region.margin);
          float margin_left = std::max(marker.center.x() - patch_min.x(), margin);
          float margin_top = std::max(patch_max.y() - marker.center.y(), margin);
          float margin_right = std::max(patch_max.x() - marker.center.x(), margin);
          float margin_bottom = std::max(marker.center.y() - patch_min.y(), margin);

          if (marker.center.x() < margin_left ||
              marker.center.x() > frame_width - margin_right ||
              marker.center.y() < margin_bottom ||
              marker.center.y() > frame_height - margin_top)
          {
            continue;
          }

          AddMarker(marker);
        }
      }
    }
  }
}

}  // namespace mv
