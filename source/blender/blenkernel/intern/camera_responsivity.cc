/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Camera spectral sensitivity (responsivity) presets and RGB-to-XYZ matrix computation.
 * Data sourced from rawtoaces-data (Apache-2.0), measured by Weta Digital.
 */

#include "BLI_math_matrix.h"

#include "BKE_camera.h"

namespace blender {

/* CIE 1931 color matching functions, 380-780nm, 5nm steps (81 entries).
 * Same data as in colormanagement.cc. */
static const float cie_color_match[81][3] = {
    {0.0014f, 0.0000f, 0.0065f}, {0.0022f, 0.0001f, 0.0105f}, {0.0042f, 0.0001f, 0.0201f},
    {0.0076f, 0.0002f, 0.0362f}, {0.0143f, 0.0004f, 0.0679f}, {0.0232f, 0.0006f, 0.1102f},
    {0.0435f, 0.0012f, 0.2074f}, {0.0776f, 0.0022f, 0.3713f}, {0.1344f, 0.0040f, 0.6456f},
    {0.2148f, 0.0073f, 1.0391f}, {0.2839f, 0.0116f, 1.3856f}, {0.3285f, 0.0168f, 1.6230f},
    {0.3483f, 0.0230f, 1.7471f}, {0.3481f, 0.0298f, 1.7826f}, {0.3362f, 0.0380f, 1.7721f},
    {0.3187f, 0.0480f, 1.7441f}, {0.2908f, 0.0600f, 1.6692f}, {0.2511f, 0.0739f, 1.5281f},
    {0.1954f, 0.0910f, 1.2876f}, {0.1421f, 0.1126f, 1.0419f}, {0.0956f, 0.1390f, 0.8130f},
    {0.0580f, 0.1693f, 0.6162f}, {0.0320f, 0.2080f, 0.4652f}, {0.0147f, 0.2586f, 0.3533f},
    {0.0049f, 0.3230f, 0.2720f}, {0.0024f, 0.4073f, 0.2123f}, {0.0093f, 0.5030f, 0.1582f},
    {0.0291f, 0.6082f, 0.1117f}, {0.0633f, 0.7100f, 0.0782f}, {0.1096f, 0.7932f, 0.0573f},
    {0.1655f, 0.8620f, 0.0422f}, {0.2257f, 0.9149f, 0.0298f}, {0.2904f, 0.9540f, 0.0203f},
    {0.3597f, 0.9803f, 0.0134f}, {0.4334f, 0.9950f, 0.0087f}, {0.5121f, 1.0000f, 0.0057f},
    {0.5945f, 0.9950f, 0.0039f}, {0.6784f, 0.9786f, 0.0027f}, {0.7621f, 0.9520f, 0.0021f},
    {0.8425f, 0.9154f, 0.0018f}, {0.9163f, 0.8700f, 0.0017f}, {0.9786f, 0.8163f, 0.0014f},
    {1.0263f, 0.7570f, 0.0011f}, {1.0567f, 0.6949f, 0.0010f}, {1.0622f, 0.6310f, 0.0008f},
    {1.0456f, 0.5668f, 0.0006f}, {1.0026f, 0.5030f, 0.0003f}, {0.9384f, 0.4412f, 0.0002f},
    {0.8544f, 0.3810f, 0.0002f}, {0.7514f, 0.3210f, 0.0001f}, {0.6424f, 0.2650f, 0.0000f},
    {0.5419f, 0.2170f, 0.0000f}, {0.4479f, 0.1750f, 0.0000f}, {0.3608f, 0.1382f, 0.0000f},
    {0.2835f, 0.1070f, 0.0000f}, {0.2187f, 0.0816f, 0.0000f}, {0.1649f, 0.0610f, 0.0000f},
    {0.1212f, 0.0446f, 0.0000f}, {0.0874f, 0.0320f, 0.0000f}, {0.0636f, 0.0232f, 0.0000f},
    {0.0468f, 0.0170f, 0.0000f}, {0.0329f, 0.0119f, 0.0000f}, {0.0227f, 0.0082f, 0.0000f},
    {0.0158f, 0.0057f, 0.0000f}, {0.0114f, 0.0041f, 0.0000f}, {0.0081f, 0.0029f, 0.0000f},
    {0.0058f, 0.0021f, 0.0000f}, {0.0041f, 0.0015f, 0.0000f}, {0.0029f, 0.0010f, 0.0000f},
    {0.0020f, 0.0007f, 0.0000f}, {0.0014f, 0.0005f, 0.0000f}, {0.0010f, 0.0004f, 0.0000f},
    {0.0007f, 0.0002f, 0.0000f}, {0.0005f, 0.0002f, 0.0000f}, {0.0003f, 0.0001f, 0.0000f},
    {0.0002f, 0.0001f, 0.0000f}, {0.0002f, 0.0001f, 0.0000f}, {0.0001f, 0.0000f, 0.0000f},
    {0.0001f, 0.0000f, 0.0000f}, {0.0001f, 0.0000f, 0.0000f}, {0.0000f, 0.0000f, 0.0000f}};

/* CIE Standard Illuminant D65, relative spectral power distribution,
 * 380-780nm, 5nm steps (81 entries). Normalized so that Y = 1.
 * Data from CIE 015:2018. */
static const float d65_spd[81] = {
    49.9755f, 52.3118f, 54.6482f, 68.7015f, 82.7549f, 87.1204f, 91.486f,  92.4589f, 93.4318f,
    90.057f,  86.6823f, 95.7736f, 104.865f, 110.936f, 117.008f, 117.41f,  117.812f, 116.336f,
    114.861f, 115.392f, 115.923f, 112.367f, 108.811f, 109.082f, 109.354f, 108.578f, 107.802f,
    106.296f, 104.79f,  106.239f, 107.689f, 106.047f, 104.405f, 104.225f, 104.046f, 102.023f,
    100.0f,   98.1671f, 96.3342f, 96.0611f, 95.788f,  92.2368f, 88.6856f, 89.3459f, 90.0062f,
    89.8026f, 89.5991f, 88.6489f, 87.6987f, 85.4936f, 83.2886f, 83.4939f, 83.6992f, 81.863f,
    80.0268f, 80.1207f, 80.2146f, 81.2462f, 82.2778f, 80.281f,  78.2842f, 74.0027f, 69.7213f,
    70.6652f, 71.6091f, 72.979f,  74.349f,  67.9765f, 61.604f,  65.7448f, 69.8856f, 72.4863f,
    75.087f,  69.3398f, 63.5927f, 55.0054f, 46.4182f, 56.6118f, 66.8054f, 65.0941f, 63.3828f,
};

/* Camera spectral sensitivity data. */
#include "camera_responsivity_data.inc"

void BKE_camera_responsivity_matrix_compute(int preset_index, float r_matrix[3][3])
{
  if (preset_index <= 0 || preset_index > CAM_TYPE_PRESET_COUNT) {
    unit_m3(r_matrix);
    return;
  }

  const float (*cam_data)[3] = camera_responsivity_presets[preset_index - 1].data;

  /* Diagonal white balance in camera RGB space, following the PhysLight approach:
   *
   *   M[i][j] = sum(CIE_i(l) * cam_j(l)) / sum(D65(l) * cam_j(l))
   *
   * This is equivalent to computing a raw CamRGB->XYZ matrix and pre-multiplying
   * by diag(1/wb_R, 1/wb_G, 1/wb_B) where wb_j = sum(D65 * cam_j).
   * The luminance normalization cancels out in the ratio. */

  float channel_sum[3] = {0.0f, 0.0f, 0.0f};
  float matrix[3][3] = {{0.0f}};

  for (int wl = 0; wl < 81; wl++) {
    for (int j = 0; j < 3; j++) {
      const float cam_sens = cam_data[wl][j] * d65_spd[wl];
      channel_sum[j] += cam_sens;
      for (int i = 0; i < 3; i++) {
        matrix[i][j] += cie_color_match[wl][i] * cam_sens;
      }
    }
  }

  for (int j = 0; j < 3; j++) {
    if (channel_sum[j] > 0.0f) {
      const float inv = 1.0f / channel_sum[j];
      for (int i = 0; i < 3; i++) {
        matrix[i][j] *= inv;
      }
    }
  }

  copy_m3_m3(r_matrix, matrix);
}

int BKE_camera_responsivity_preset_count()
{
  return CAM_TYPE_PRESET_COUNT;
}

const char *BKE_camera_responsivity_preset_name(int index)
{
  if (index < 0 || index >= CAM_TYPE_PRESET_COUNT) {
    return nullptr;
  }
  return camera_responsivity_presets[index].name;
}

}  // namespace blender
