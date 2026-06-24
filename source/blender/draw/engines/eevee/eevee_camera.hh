/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup eevee
 */

#include "BLI_math_base_c.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_c.hh"

#include "BKE_camera.h"

#include "eevee_camera_shared.hh"

namespace blender::eevee {

class Instance;

inline float4x4 cubeface_mat(int face)
{
  switch (face) {
    default:
    case 0:
      /* Pos X */
      return float4x4({0.0f, 0.0f, -1.0f, 0.0f},
                      {0.0f, -1.0f, 0.0f, 0.0f},
                      {-1.0f, 0.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, 0.0f, 1.0f});
    case 1:
      /* Neg X */
      return float4x4({0.0f, 0.0f, 1.0f, 0.0f},
                      {0.0f, -1.0f, 0.0f, 0.0f},
                      {1.0f, 0.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, 0.0f, 1.0f});
    case 2:
      /* Pos Y */
      return float4x4({1.0f, 0.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, -1.0f, 0.0f},
                      {0.0f, 1.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, 0.0f, 1.0f});
    case 3:
      /* Neg Y */
      return float4x4({1.0f, 0.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, 1.0f, 0.0f},
                      {0.0f, -1.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, 0.0f, 1.0f});
    case 4:
      /* Pos Z */
      return float4x4({1.0f, 0.0f, 0.0f, 0.0f},
                      {0.0f, -1.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, -1.0f, 0.0f},
                      {0.0f, 0.0f, 0.0f, 1.0f});
    case 5:
      /* Neg Z */
      return float4x4({-1.0f, 0.0f, 0.0f, 0.0f},
                      {0.0f, -1.0f, 0.0f, 0.0f},
                      {0.0f, 0.0f, 1.0f, 0.0f},
                      {0.0f, 0.0f, 0.0f, 1.0f});
  }
}

inline void cubeface_winmat_get(float4x4 &winmat, float near, float far)
{
  /* Simple 90 degree FOV projection. */
  perspective_m4(winmat.ptr(), -near, near, -near, near, near, far);
}

struct PanoramicFaceRect {
  float2 min = float2(-1.0f);
  float2 max = float2(1.0f);
};

inline bool panoramic_uses_fisheye_face_rect(eCameraType type)
{
  return ELEM(
      type, CAMERA_PANO_EQUIDISTANT, CAMERA_PANO_EQUISOLID, CAMERA_PANO_FISHEYE_LENS_POLYNOMIAL);
}

inline PanoramicFaceRect panoramic_face_rect_get(const CameraData &cam, int face_id)
{
  PanoramicFaceRect rect;

  if (panoramic_uses_fisheye_face_rect(cam.type)) {
    const float half_fov = clamp_f(cam.fisheye_fov * 0.5f, 0.0f, float(M_PI));
    constexpr float half_pi = float(M_PI_2);
    constexpr float half_pi_eps = 1.0e-4f;

    if (half_fov < half_pi - half_pi_eps) {
      const float sin_half_fov = sinf(half_fov);
      const float cos_half_fov = cosf(half_fov);
      const float tan_half_fov = tanf(half_fov);

      switch (face_id) {
        case 0:
        case 1: {
          const float x_min = cos_half_fov / max_ff(sin_half_fov, 1.0e-8f);
          const float y_extent = min_ff(sqrtf(max_ff(square_f(tan_half_fov) - 1.0f, 0.0f)), 1.0f);
          rect.min = (face_id == 0) ? float2(x_min, -y_extent) : float2(-1.0f, -y_extent);
          rect.max = (face_id == 0) ? float2(1.0f, y_extent) : float2(-x_min, y_extent);
          break;
        }
        case 2:
        case 3: {
          const float y_min = cos_half_fov / max_ff(sin_half_fov, 1.0e-8f);
          const float x_extent = min_ff(sqrtf(max_ff(square_f(tan_half_fov) - 1.0f, 0.0f)), 1.0f);
          rect.min = (face_id == 2) ? float2(-x_extent, -1.0f) : float2(-x_extent, y_min);
          rect.max = (face_id == 2) ? float2(x_extent, -y_min) : float2(x_extent, 1.0f);
          break;
        }
        case 5: {
          const float extent = min_ff(tan_half_fov, 1.0f);
          rect.min = float2(-extent);
          rect.max = float2(extent);
          break;
        }
        default:
          break;
      }
    }
    else if (half_fov <= half_pi + half_pi_eps) {
      switch (face_id) {
        case 0:
          rect.min = float2(0.0f, -1.0f);
          rect.max = float2(1.0f, 1.0f);
          break;
        case 1:
          rect.min = float2(-1.0f, -1.0f);
          rect.max = float2(0.0f, 1.0f);
          break;
        case 2:
          rect.min = float2(-1.0f, -1.0f);
          rect.max = float2(1.0f, 0.0f);
          break;
        case 3:
          rect.min = float2(-1.0f, 0.0f);
          rect.max = float2(1.0f, 1.0f);
          break;
        default:
          break;
      }
    }
  }

  const float overscan = max_ff(cam.panoramic_view_overscan, 1.0f);
  const float2 center = (rect.min + rect.max) * 0.5f;
  const float2 half_extent = (rect.max - rect.min) * (0.5f * overscan);
  rect.min = center - half_extent;
  rect.max = center + half_extent;
  return rect;
}

/* -------------------------------------------------------------------- */
/** \name CameraData operators
 * \{ */

inline bool operator==(const CameraData &a, const CameraData &b)
{
  return compare_m4m4(a.persmat.ptr(), b.persmat.ptr(), FLT_MIN) && (a.uv_scale == b.uv_scale) &&
         (a.uv_bias == b.uv_bias) && (a.equirect_scale == b.equirect_scale) &&
         (a.equirect_bias == b.equirect_bias) && (a.fisheye_fov == b.fisheye_fov) &&
         (a.fisheye_lens == b.fisheye_lens) && (a.fisheye_sensor == b.fisheye_sensor) &&
         (a.fisheye_polynomial_bias == b.fisheye_polynomial_bias) &&
         (a.fisheye_polynomial_coefficients == b.fisheye_polynomial_coefficients) &&
         (a.central_cylindrical_range == b.central_cylindrical_range) &&
         (a.panoramic_view_mask == b.panoramic_view_mask) && (a.type == b.type);
}

inline bool operator!=(const CameraData &a, const CameraData &b)
{
  return !(a == b);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Camera
 * \{ */

/**
 * Point of view in the scene. Can be init from viewport or camera object.
 */
class Camera {
 private:
  Instance &inst_;

  CameraData &data_;

  struct {
    float3 center;
    float radius;
  } bound_sphere;

  float overscan_ = -1.0f;
  bool overscan_changed_;
  /** Whether or not the camera was synced from a camera object. */
  bool is_camera_object_ = false;
  /** Just for tracking camera changes, use Instance::camera_orig_object for data access. */
  Object *last_camera_object_ = nullptr;
  bool camera_changed_ = false;

 public:
  Camera(Instance &inst, CameraData &data) : inst_(inst), data_(data) {};
  ~Camera() {};

  void init();
  void sync();

  /**
   * Getters
   */
  const CameraData &data_get() const
  {
    BLI_assert(data_.initialized);
    return data_;
  }
  bool is_panoramic() const
  {
    return eevee::is_panoramic(data_.type);
  }
  bool is_orthographic() const
  {
    return data_.type == CAMERA_ORTHO;
  }
  bool is_perspective() const
  {
    return data_.type == CAMERA_PERSP;
  }
  bool is_camera_object() const
  {
    return is_camera_object_;
  }
  const float3 &position() const
  {
    return data_.viewinv.location();
  }
  const float3 &forward() const
  {
    return data_.viewinv.z_axis();
  }
  const float3 &bound_center() const
  {
    return bound_sphere.center;
  }
  const float &bound_radius() const
  {
    return bound_sphere.radius;
  }
  float overscan() const
  {
    return overscan_;
  }
  bool overscan_changed() const
  {
    return overscan_changed_;
  }
  bool camera_changed() const
  {
    return camera_changed_;
  }

 private:
  void update_bounds();

  float4x4 projection_crop_matrix(int2 film_offset, int2 film_extent, int2 display_extent);
  float4x4 projection_overscan_matrix(int2 film_extent, int2 film_overscan);
};

/** \} */

}  // namespace blender::eevee
