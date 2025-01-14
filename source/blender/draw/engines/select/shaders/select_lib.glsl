/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#ifndef SELECT_ENABLE
/* Avoid requesting the select_id when not in selection mode. */
#  define select_id_set(select_id)
#  define select_id_output(select_id)

#elif defined(GPU_VERTEX_SHADER)

void select_id_set(int id)
{
  /* Declared in the create info. */
  select_id = id;
}

#elif defined(GPU_FRAGMENT_SHADER)

void select_id_output(int id)
{
  if (id == -1) {
    /* Invalid index */
    return;
  }

  /* TODO(Miguel Pozo): This degrades precission. Does it matter in practice? */
  float select_depth = gl_FragCoord.z + (select_info_buf.in_front ? 0.0f : 1.0f);

  if (select_info_buf.mode == SELECT_ALL) {
    /* Set the bit of the select id in the bitmap. */
    atomicOr(out_select_buf[id / 32u], 1u << (uint(id) % 32u));
  }
  else if (select_info_buf.mode == SELECT_PICK_ALL) {
    /* Stores the nearest depth for this select id. */
    atomicMin(out_select_buf[id], floatBitsToUint(select_depth));
  }
  else if (select_info_buf.mode == SELECT_PICK_NEAREST) {
    /* Stores the nearest depth with the distance to the cursor. */

    /* Distance function to the cursor. Currently a simple pixel ring distance. */
    ivec2 coord = abs(ivec2(gl_FragCoord.xy) - select_info_buf.cursor);
    uint dist = uint(max(coord.x, coord.y));

    uint depth = uint(select_depth * float(0x00FFFFFFu));

    /* Reject hits outside of valid range. */
    if (dist < 0xFFu) {
      /* Packed values to ensure the atomicMin is performed on the whole result. */
      atomicMin(out_select_buf[id], (depth << 8u) | dist);
    }
  }
}

#endif
