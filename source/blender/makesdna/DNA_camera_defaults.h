/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name Camera Struct
 * \{ */

#define _DNA_DEFAULT_CameraDOFSettings \
  { \
    .aperture_fstop = 2.8f, \
    .aperture_ratio = 1.0f, \
    .focus_distance = 10.0f, \
  }

#define _DNA_DEFAULT_CameraStereoSettings \
  { \
    .interocular_distance = 0.065f, \
    .convergence_distance = 30.0f * 0.065f, \
    .pole_merge_angle_from = DEG2RADF(60.0f), \
    .pole_merge_angle_to = DEG2RADF(75.0f), \
  }

#define _DNA_DEFAULT_Camera \
  { \
    .lens = 50.0f, \
    .sensor_x = DEFAULT_SENSOR_WIDTH, \
    .sensor_y = DEFAULT_SENSOR_HEIGHT, \
    .clip_start = 0.1f, \
    .clip_end = 1000.0f, \
    .drawsize = 1.0f, \
    .ortho_scale = 6.0, \
    .flag = CAM_SHOWPASSEPARTOUT, \
    .passepartalpha = 0.5f, \
 \
    .panorama_type = CAM_PANORAMA_FISHEYE_EQUISOLID,\
    .fisheye_fov = M_PI,\
    .fisheye_lens = 10.5f,\
    .latitude_min = -0.5f * (float)M_PI,\
    .latitude_max = 0.5f * (float)M_PI,\
    .longitude_min = -M_PI,\
    .longitude_max = M_PI,\
    /* Fit to match default projective camera with focal_length 50 and sensor_width 36. */ \
    .fisheye_polynomial_k0 = -1.1735143712967577e-05f,\
    .fisheye_polynomial_k1 = -0.019988736953434998f,\
    .fisheye_polynomial_k2 = -3.3525322965709175e-06f,\
    .fisheye_polynomial_k3 = 3.099275275886036e-06f,\
    .fisheye_polynomial_k4 = -2.6064646454854524e-08f,\
    .central_cylindrical_range_u_min = DEG2RADF(-180.0f),\
    .central_cylindrical_range_u_max = DEG2RADF(180.0f),\
    .central_cylindrical_range_v_min = -1.0f,\
    .central_cylindrical_range_v_max = 1.0f,\
    .central_cylindrical_radius = 1.0f,\
    /* Default Aria SLAM camera projection calibration */ \
    .fisheye624_f =  240.96908202503016128f,\
    .fisheye624_cx = 319.30031322283957707f,\
    .fisheye624_cy = 239.70226462142591117f,\
    .fisheye624_k0 = -0.00029975978022917562074f,\
    .fisheye624_k1 = 0.025925353248573888842f,\
    .fisheye624_k2 = 0.0049689703789174387294f,\
    .fisheye624_k3 = -0.0082339337266616879907f,\
    .fisheye624_k4 = -0.0058290815323180505958f,\
    .fisheye624_k5 = 0.0026384817055371189917f,\
    .fisheye624_p0 = 0.00016612194528025018398f,\
    .fisheye624_p1 = 2.3049914803609829601e-05f,\
    .fisheye624_s0 = -0.00025728595469903830411f,\
    .fisheye624_s1 = -3.7265140092139775881e-05f,\
    .fisheye624_s2 = -0.0006244819671333829f,\
    .fisheye624_s3 = -6.834843688531277463e-05f,\
 \
    .dof = _DNA_DEFAULT_CameraDOFSettings, \
 \
    .stereo = _DNA_DEFAULT_CameraStereoSettings, \
  }

/** \} */

/* clang-format on */
