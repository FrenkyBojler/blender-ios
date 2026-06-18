/* SPDX-FileCopyrightText: 2019-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Overlay anti-aliasing:
 *
 * Single sample per pixel screen-space AA pass for wires and wireframe
 * overlays, refer to `overlay_antialiasing.hh` for a breakdown. This
 * can be toggled in `Settings > Viewport > Smooth Wires > Overlay`.
 */

#pragma once

#include "gpu_shader_compat.hh"
#include "gpu_shader_fullscreen_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_constants_lib.glsl"
#include "infos/overlay_common_infos.hh"
#include "overlay_shader_shared.hh"

SHADER_LIBRARY_CREATE_INFO(draw_globals)

namespace overlay::antialiasing {

/* Per-pixel line data, unpacked as a direction and covered distance. */
struct Line {
  float2 dir;
  float dist;
  float dist_raw;

  static Line zero()
  {
    return {.dir = float2(0.0f), .dist = 0.0f, .dist_raw = 0.0f};
  }

  static Line decode(float2 data)
  {
    /* Unpack distance to edge, remove 0.1f boundary that differentiates cleared pixels. */
    float dist = (data.y - 0.5f) * 2.5f;

    /* Recover perpendicular vector from packed sin_theta. */
    float sin_theta = (data.x - 0.5f) * 2.0f;
    float cos_theta = cos_from_sin(sin_theta);
    float2 perp = normalize(float2(sin_theta, cos_theta));

    return {
        .dir = perp,
        .dist = dist,
        .dist_raw = data.y,
    };
  }

  bool is_valid() const
  {
    return dist_raw != 0.0f && dist_raw != 1.0f;
  }

  bool is_blocked() const
  {
    return dist_raw == 1.0f;
  }
};

struct TexelData {
  float4 color;
  float depth;
  Line line;

  static TexelData zero()
  {
    return {.color = float4(0.0f), .depth = 1.0f, .line = Line::zero()};
  }
};

struct Resources {
  [[legacy_info]] ShaderCreateInfo draw_globals;

  [[sampler(0)]] const sampler2DDepth depth_tx;
  [[sampler(1)]] const sampler2D color_tx;
  [[sampler(2)]] const sampler2D line_tx;

  [[push_constant]] const bool do_smooth_lines;
  [[push_constant]] const bool do_background_fetch;

  TexelData fetch_texel(int2 texel, int2 offset)
  {
    int2 texel_actual = texel + offset;
    TexelData texel_data = {
        .color = texelFetch(color_tx, texel_actual, 0),
        .depth = texelFetch(depth_tx, texel_actual, 0).r,
        .line = Line::decode(texelFetch(line_tx, texel_actual, 0).rg),
    };
    return texel_data;
  }
};

/**
 * Coverage of a line onto a sample that is `distance_to_Line` pixels from the line.
 * Here, `line_kernel_size` is the inner size of the line with 100% coverage.
 */
template<typename T>
T line_coverage(T distance_to_line, float line_kernel_size, bool do_smooth_lines)
{
  if (do_smooth_lines) {
    return smoothstep(
        LINE_SMOOTH_END, LINE_SMOOTH_START, abs(distance_to_line) - line_kernel_size);
  }
  return step(-0.5f, line_kernel_size - abs(distance_to_line));
}

template float line_coverage<float>(float, float, bool);
template float4 line_coverage<float4>(float4, float, bool);

/**
 * Return the furthest non-line texel in the neighboring crosshair.
 */
TexelData furthest_neighbor(TexelData center, TexelData neighbors[4])
{
  TexelData furthest = center;
  for (int i = 0; i < 4; ++i) {
    if (neighbors[i].depth > furthest.depth && !neighbors[i].line.is_valid()) {
      furthest = neighbors[i];
    }
  }
  return !furthest.line.is_valid() ? furthest : TexelData::zero();
}

/**
 * Compute distance-to-line for one of the neighboring crosshair pixels, dependent
 * on whether that pixel has influence or not; in which case distance is set to
 * a maximal value.
 */
float neighbor_dist(const TexelData &neighbor, int2 offset)
{
  bool is_dir_horizontal = abs(neighbor.line.dir.x) > abs(neighbor.line.dir.y);
  bool is_ofs_horizontal = offset.x != 0;

  if (!neighbor.line.is_valid() || is_ofs_horizontal != is_dir_horizontal) {
    return 1e10f; /* No line. */
  }

  /* Add projection of the line direction onto a unit vector. */
  return neighbor.line.dist + dot(float2(offset), -neighbor.line.dir);
}

/**
 * Blend the neighbor pixel onto the target pixel, based on the pixels'
 * relative depths doing alpha-over or alpha-under. The resulting pixel's
 * depth is then adjusted to the closest depth.
 */
void neighbor_blend(TexelData neighbor,
                    TexelData &target,
                    float line_coverage,
                    bool blend_over_background)
{
  /* Special value on neighbor indicates it should not affect pixels around it. */
  if (neighbor.line.is_blocked() || line_coverage == 0.0f) {
    return;
  }

  /* Update target color using alpha-over/alpha-under, dependent on which
   * pixel is closest. */
  bool target_over_neighbor = target.depth < neighbor.depth;
  float4 over = target_over_neighbor ? target.color : neighbor.color;
  float4 under = target_over_neighbor ? neighbor.color : target.color;
  if (blend_over_background) {
    /* With background, avoid working with pre-multiplied alpha. */
    under.a *= (1.0f - over.a);
    target.color = (over * over.a + under * under.a) / (over.a + under.a);
  }
  else {
    /* Without background, do additive alpha over the current framebuffer value. */
    target.color = over + under * (1.0f - over.a);
  }

  /* Update tracked depth value to closest. */
  if (!target_over_neighbor) {
    target.depth = neighbor.depth;
  }
}

[[vertex]] void vert_main([[vertex_id]] const int &vert_id, [[position]] float4 &vert)
{
  fullscreen_vertex(vert_id, vert);
}

struct FragOut {
  [[frag_color(0)]] float4 color;
};

[[fragment]] void frag_main([[frag_coord]] const float4 &frag_coord,
                            [[resource_table]] Resources &srt,
                            [[out]] FragOut &frag)
{
  int2 texel = int2(frag_coord.xy);

  /* Fetch data at the center pixel. */
  TexelData center = srt.fetch_texel(texel, int2(0));

  /* Store until end of function; does the center pixel have alpha? */
  bool original_center_has_alpha = center.color.a < 1.0f;

  /* Guard; check if no expansion or AA should be applied. */
  if (!srt.do_smooth_lines && center.line.dist <= 1.0f) {
    frag.color = center.color;
    return;
  }

  /* Fetch data for a cross of neighboring pixels. */
  TexelData neighbors[] = {srt.fetch_texel(texel, int2(1, 0)),
                           srt.fetch_texel(texel, int2(-1, 0)),
                           srt.fetch_texel(texel, int2(0, 1)),
                           srt.fetch_texel(texel, int2(0, -1))};

  /* Fetch the furthest available among center+neighboring pixels. This is only fetched
   * in cases like x-ray mode, where some overlays write color as background. */
  TexelData background = srt.do_background_fetch ? furthest_neighbor(center, neighbors) :
                                                   TexelData::zero();

  float4 neighbor_dists = float4(neighbor_dist(neighbors[0], int2(1, 0)),
                                 neighbor_dist(neighbors[1], int2(-1, 0)),
                                 neighbor_dist(neighbors[2], int2(0, 1)),
                                 neighbor_dist(neighbors[3], int2(0, -1)));

  /* Compute per-neighbor line coverage */
  float line_kernel = theme.sizes.pixel * 0.5f - 0.5f;

  /* Blend target color over background based on center pixel's line coverage. */
  if (center.line.is_valid()) {
    float coverage = line_coverage(center.line.dist, line_kernel, srt.do_smooth_lines);
    center.color = mix(background.color, center.color, coverage);
  }

  float4 coverage = line_coverage(neighbor_dists, line_kernel, srt.do_smooth_lines);
  for (int i = 0; i < 4; i++) [[unroll]] {
    /* Blend neighbor colors over background based on their respective line coverages. */
    neighbors[i].color = mix(background.color, neighbors[i].color, coverage[i]);

    /* We don't order fragments; intsead blending as alpha-over/alpha-under based on
     * the tracked depth of each neighbor, using the center pixel as reference value. */
    neighbor_blend(neighbors[i], center, coverage[i], background.color.a != 0.0f);
  }

#if 1
  /* Fix aliasing issue with really dense meshes and 1 pixel sized lines. */
  if (!original_center_has_alpha && center.line.is_valid() && line_kernel < 0.45f) {
    float4 lines_raw = float4(neighbors[0].line.dist_raw,
                              neighbors[1].line.dist_raw,
                              neighbors[2].line.dist_raw,
                              neighbors[3].line.dist_raw);
    float blend = dot(float4(0.25f), step(1e-2f, lines_raw));

    /* Only do blend if there are more than 2 neighbors, to avoid losing too much AA. */
    blend = clamp(blend * 2.0f - 1.0f, 0.0f, 1.0f);
    center.color = mix(center.color, center.color / center.color.a, blend);
  }
#endif

  frag.color = center.color;
}

PipelineGraphic pipeline(vert_main, frag_main);

}  // namespace overlay::antialiasing
