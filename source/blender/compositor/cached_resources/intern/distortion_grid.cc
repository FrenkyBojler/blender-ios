/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdint>
#include <memory>

#include "BLI_hash.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_defaults.h"
#include "DNA_movieclip_types.h"
#include "DNA_tracking_types.h"

#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "COM_context.hh"
#include "COM_distortion_grid.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

/* --------------------------------------------------------------------
 * Distortion Grid Key.
 */

DistortionGridKey::DistortionGridKey(const MovieTrackingCamera &camera,
                                     int2 size,
                                     DistortionType type,
                                     int2 calibration_size)
    : camera(camera), size(size), type(type), calibration_size(calibration_size)
{
}

uint64_t DistortionGridKey::hash() const
{
  return get_default_hash(
      BKE_tracking_camera_distortion_hash(&camera), size, type, calibration_size);
}

bool operator==(const DistortionGridKey &a, const DistortionGridKey &b)
{
  return BKE_tracking_camera_distortion_equal(&a.camera, &b.camera) && a.size == b.size &&
         a.type == b.type && a.calibration_size == b.calibration_size;
}

/* --------------------------------------------------------------------
 * Distortion Grid.
 */

DistortionGrid::DistortionGrid(Context &context,
                               MovieClip *movie_clip,
                               Domain domain,
                               DistortionType type,
                               int2 calibration_size)
    : result(context.create_result(ResultType::Float2, ResultPrecision::Full))
{
  MovieDistortion *distortion = BKE_tracking_distortion_new(
      &movie_clip->tracking, calibration_size.x, calibration_size.y);

  /* Compute how much the image would extend outside each of its bounds due to distortion. */
  int right_delta;
  int left_delta;
  int bottom_delta;
  int top_delta;
  BKE_tracking_distortion_bounds_deltas(distortion,
                                        domain.size,
                                        calibration_size,
                                        type == DistortionType::Undistort,
                                        &right_delta,
                                        &left_delta,
                                        &bottom_delta,
                                        &top_delta);

  /* Clamp deltas to avoid excessive memory requirements in case of extreme distortion. */
  right_delta = math::min(right_delta, domain.size.x);
  left_delta = math::min(left_delta, domain.size.x);
  bottom_delta = math::min(bottom_delta, domain.size.y);
  top_delta = math::min(top_delta, domain.size.y);

  Domain output_domain = domain;
  output_domain.size = domain.size + int2(right_delta + left_delta, bottom_delta + top_delta);
  output_domain.data_offset = int2(left_delta, bottom_delta);
  this->result.allocate_texture(output_domain, false, ResultStorageType::CPU);

  parallel_for(this->result.domain().size, [&](const int2 texel) {
    const float2 display_coordinates = float2(texel - output_domain.data_offset) + 0.5f;
    const float2 normalized_coordinates = display_coordinates / float2(domain.display_size);
    const float2 calibrated_coordinates = normalized_coordinates * float2(calibration_size);

    float2 distorted_coordinates;
    if (type == DistortionType::Undistort) {
      BKE_tracking_distortion_undistort_v2(
          distortion, calibrated_coordinates, distorted_coordinates);
    }
    else {
      BKE_tracking_distortion_distort_v2(
          distortion, calibrated_coordinates, distorted_coordinates);
    }

    const float2 distorted_normalized_coordinates = distorted_coordinates /
                                                    float2(calibration_size);
    const float2 distorted_display_coordinates = distorted_normalized_coordinates *
                                                 float2(domain.display_size);
    const float2 distorted_data_coordinates = distorted_display_coordinates +
                                              float2(domain.data_offset);
    const float2 sampling_coordinates = distorted_data_coordinates / float2(domain.size);
    this->result.store_pixel(texel, sampling_coordinates);
  });

  BKE_tracking_distortion_free(distortion);

  if (context.use_gpu()) {
    const Result gpu_result = this->result.upload_to_gpu(false);
    this->result.release();
    this->result = gpu_result;
  }
}

DistortionGrid::~DistortionGrid()
{
  this->result.release();
}

/* --------------------------------------------------------------------
 * Distortion Grid Container.
 */

void DistortionGridContainer::reset()
{
  /* First, delete all resources that are no longer needed. */
  map_.remove_if([](auto item) { return !item.value->needed; });

  /* Second, reset the needed status of the remaining resources to false to ready them to track
   * their needed status for the next evaluation. */
  for (auto &value : map_.values()) {
    value->needed = false;
  }
}

static int2 get_movie_clip_size(MovieClip *movie_clip, int frame_number)
{
  MovieClipUser user = *DNA_struct_default_get(MovieClipUser);
  BKE_movieclip_user_set_frame(&user, frame_number);

  int2 size;
  BKE_movieclip_get_size(movie_clip, &user, &size.x, &size.y);

  return size;
}

Result &DistortionGridContainer::get(
    Context &context, MovieClip *movie_clip, Domain domain, DistortionType type, int frame_number)
{
  const int2 calibration_size = get_movie_clip_size(movie_clip, frame_number);

  const DistortionGridKey key(movie_clip->tracking.camera, domain.size, type, calibration_size);

  auto &distortion_grid = *map_.lookup_or_add_cb(key, [&]() {
    return std::make_unique<DistortionGrid>(context, movie_clip, domain, type, calibration_size);
  });

  distortion_grid.needed = true;
  return distortion_grid.result;
}

}  // namespace blender::compositor
