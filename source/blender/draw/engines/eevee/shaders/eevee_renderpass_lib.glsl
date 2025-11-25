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
  int total_len = uniform_buf.render_pass.aovs.color_len + uniform_buf.render_pass.aovs.value_len;
  for (int i = 0; 4 * i < AOV_MAX && 4 * i < total_len; i++) {
    /* Search hashes in uint4 packs; 4 comparisons at once to find the index of a candidate. */
    bool4 cmp_mask = equal(uniform_buf.render_pass.aovs.hash[i], uint4(hash));
    if (!any(cmp_mask)) {
      continue;
    }
    /* Left-reduce of `cmp_mask` to find the index of candidate. */
    int j = 4 * i + (cmp_mask[0] ? 0 : (cmp_mask[1] ? 1 : (cmp_mask[2] ? 2 : 3)));

    /* Value hashes are stored after color hashes, so we have to subtract for the value offset. */
    bool is_value = j >= uniform_buf.render_pass.aovs.color_len;
    if (is_value) {
      j += uniform_buf.render_pass.value_len - uniform_buf.render_pass.aovs.color_len;
      imageStoreFast(rp_value_img, int3(int2(gl_FragCoord.xy), j), float4(value));
    }
    else {
      j += uniform_buf.render_pass.color_len;
      imageStoreFast(rp_color_img, int3(int2(gl_FragCoord.xy), j), color);
    }
    return;
  }
#endif
}
