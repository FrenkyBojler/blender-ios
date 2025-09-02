/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define CMP_NODE_SETALPHA_MODE_APPLY 0.0f
#define CMP_NODE_SETALPHA_MODE_REPLACE_ALPHA 1.0f

void node_composite_set_alpha(float4 color, float alpha, float mode, out float4 result)
{
  if (mode == CMP_NODE_SETALPHA_MODE_APPLY) {
    result = color * alpha;
  }
  else if (mode == CMP_NODE_SETALPHA_MODE_REPLACE_ALPHA) {
    result = float4(color.rgb, alpha);
  }
}

#undef CMP_NODE_SETALPHA_MODE_APPLY
#undef CMP_NODE_SETALPHA_MODE_REPLACE_ALPHA
