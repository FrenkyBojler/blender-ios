/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#pragma once

struct ImBuf;
struct Strip;
struct Scene;
struct RenderData;

namespace blender::seq {

void final_image_cache_put(const RenderData *context, float timeline_frame, ImBuf *image);

ImBuf *final_image_cache_get(const RenderData *context, float timeline_frame);

void final_image_cache_invalidate_frame_range(Scene *scene,
                                              const float timeline_frame_start,
                                              const float timeline_frame_end);

void final_image_cache_clear(Scene *scene);
void final_image_cache_destroy(Scene *scene);

size_t final_image_cache_calc_memory_size(Scene *scene);
bool final_image_cache_evict(Scene *scene);
void final_image_cache_tick(Scene *scene);

}  // namespace blender::seq
