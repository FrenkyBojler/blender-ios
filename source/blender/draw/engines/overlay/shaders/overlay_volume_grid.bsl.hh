/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view_infos.hh"

namespace workbench::overlay_volume_grid {

struct FragOut {
  [[frag_color(0)]]
  float4 color;
};

[[vertex]]
void vert_main([[vertex_id]]
               const int &vert_id,
               [[position]]
               float4 &out_pos)
{
  switch (vert_id % 3) {
    case 0: { out_pos = float4(0, 0, 0, 1); break; }
    case 1: { out_pos = float4(900, 900, 0, 1); break; }
    default: { out_pos = float4(900, 0, 0, 1); break; }
  }
}

[[fragment]]
void frag_main([[frag_coord]]
               const float4 &frag_coord,
               [[out]]
               FragOut &frag)
{
  assert(false);
  frag.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
}

PipelineGraphic pipline(vert_main, frag_main);

}  // namespace workbench::overlay_volume_grid