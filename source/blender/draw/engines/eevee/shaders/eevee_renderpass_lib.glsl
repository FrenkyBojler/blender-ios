/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "infos/eevee_common_infos.hh"

SHADER_LIBRARY_CREATE_INFO(eevee_render_pass_out)

void output_renderpass_color(int id, float4 color)
{
#if defined(MAT_RENDER_PASS_SUPPORT) && defined(GPU_FRAGMENT_SHADER)
  if (id >= 0) {
    int2 texel = int2(gl_FragCoord.xy);
    imageStoreFast(rp_color_img, int3(texel, id), color);
  }
#endif
}

void output_renderpass_value(int id, float value)
{
#if defined(MAT_RENDER_PASS_SUPPORT) && defined(GPU_FRAGMENT_SHADER)
  if (id >= 0) {
    int2 texel = int2(gl_FragCoord.xy);
    imageStoreFast(rp_value_img, int3(texel, id), float4(value));
  }
#endif
}

void clear_aovs()
{
#if defined(MAT_RENDER_PASS_SUPPORT) && defined(GPU_FRAGMENT_SHADER)
  for (int i = 0; i < AOV_MAX && i < uniform_buf.render_pass.aovs.color_len; i++) {
    output_renderpass_color(uniform_buf.render_pass.color_len + i, float4(0));
  }
  for (int i = 0; i < AOV_MAX && i < uniform_buf.render_pass.aovs.value_len; i++) {
    output_renderpass_value(uniform_buf.render_pass.value_len + i, 0.0f);
  }
#endif
}

void output_aov(float4 color, float value, uint hash)
{
#if defined(MAT_RENDER_PASS_SUPPORT) && defined(GPU_FRAGMENT_SHADER)
  uint total_len = uniform_buf.render_pass.aovs.color_len + uniform_buf.render_pass.aovs.value_len;

  /* Search hashes in uint4 packs; 4 comparisons at once to find the index of a candidate. */
  uint aov_index;
  for (aov_index = 0u; aov_index < AOV_MAX && aov_index < total_len; aov_index += 4u) {
    bool4 cmp_mask = equal(uniform_buf.render_pass.aovs.hash[aov_index >> 2u], uint4(hash));
    if (any(cmp_mask)) {
      /* Left-reduce of `cmp_mask` to find the index of candidate. */
      aov_index += (cmp_mask[0] ? 0u : (cmp_mask[1] ? 1u : (cmp_mask[2] ? 2u : 3u)));
      break;
    }
  }

  /* If a hash was found, output to texture array layer. */
  if (aov_index != AOV_MAX) {
    /* Value hashes are stored after color hashes, so we have to subtract for the value offset. */
    bool is_value = aov_index >= uniform_buf.render_pass.aovs.color_len;
    if (is_value) {
      aov_index += uniform_buf.render_pass.value_len - uniform_buf.render_pass.aovs.color_len;
      imageStoreFast(rp_value_img, int3(int2(gl_FragCoord.xy), aov_index), float4(value));
    }
    else {
      aov_index += uniform_buf.render_pass.color_len;
      imageStoreFast(rp_color_img, int3(int2(gl_FragCoord.xy), aov_index), color);
    }
  }
#endif
}
