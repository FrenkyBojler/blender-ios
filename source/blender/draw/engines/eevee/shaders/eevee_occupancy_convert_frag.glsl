/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Convert hit list to occupancy bit-field for the material pass.
 */

#include "infos/eevee_volume_info.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_volume_occupancy_convert)

#include "eevee_occupancy_lib.glsl"

bool is_front_face_hit(float stored_hit_depth)
{
  return stored_hit_depth > 0.0f;
}

void main()
{
  float hit_depths[VOLUME_HIT_DEPTH_MAX];
  float hit_ordered[VOLUME_HIT_DEPTH_MAX + 1];
  int hit_index[VOLUME_HIT_DEPTH_MAX];

  int2 texel = int2(gl_FragCoord.xy);

  int hit_count = int(imageLoad(hit_count_img, texel).x);
  hit_count = min(hit_count, VOLUME_HIT_DEPTH_MAX);

  if (hit_count == 0) {
    return;
  }

  /* Clear the texture for next layer / frame. */
  imageStore(hit_count_img, texel, uint4(0));

  for (int i = 0; i < hit_count; i++) {
    hit_depths[i] = imageLoad(hit_depth_img, int3(texel, i)).r;
  }

  for (int i = 0; i < hit_count; i++) {
    hit_index[i] = 0;
    for (int j = 0; j < hit_count; j++) {
      hit_index[i] += int(abs(hit_depths[i]) > abs(hit_depths[j]));
    }
  }

  for (int i = 0; i < hit_count; i++) {
    hit_ordered[hit_index[i]] = hit_depths[i];
  }

#if 0 /* Debug. Need to adjust the qualifier of the texture adjusted. */
  for (int i = 0; i < hit_count; i++) {
    imageStore(hit_depth_img, int3(texel, i), float4(hit_ordered[i]));
  }
#endif

  /* Convert to occupancy bits. */
  occupancy::Bits occupancy = occupancy::occupancy_new();
  /* Consider camera in volume if first hit is a backface. */
  bool camera_in_volume = !is_front_face_hit(hit_ordered[0]);
  int stack_depth = int(camera_in_volume);
  /* Add artificial back-facing hit to close volumes we entered but never exited.
   * Fixes issues with non-manifold meshes or things like water planes. */
  hit_ordered[hit_count] = -1.0f;
  /* Bit index of the last interface. */
  int last_bit = 0;
  for (int i = 0; i <= hit_count; i++) {
    int occupancy_bit_n = occupancy::bit_index_from_depth(abs(hit_ordered[i]),
                                                          uniform_buf.volumes.tex_size.z);
    int bit_start = last_bit;
    int bit_count = occupancy_bit_n - last_bit;
    last_bit = occupancy_bit_n;

    /* occupancy::Bits is cleared by default. No need to do anything for empty regions. */
    if (stack_depth != 0) {
      occupancy = occupancy::set_bits_high(occupancy, bit_start, bit_count);
    }

    if (is_front_face_hit(hit_ordered[i])) {
      stack_depth++;
    }
    else {
      stack_depth = max(stack_depth - 1, 0);
    }
  }

  /* Write the occupancy bits */
  for (int i = 0; i < imageSize(occupancy_img).z; i++) {
    if (occupancy.bits[i] != 0u) {
      /* NOTE: Doesn't have to be atomic but we need to blend with other method. */
      imageAtomicOr(occupancy_img, int3(texel, i), occupancy.bits[i]);
    }
  }
}
