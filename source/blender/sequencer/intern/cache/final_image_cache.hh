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

namespace blender::seq {

void final_image_cache_put(Scene *scene, float timeline_frame, ImBuf *image);

ImBuf *final_image_cache_get(Scene *scene, float timeline_frame);

void final_image_cache_invalidate_frame_range(Scene *scene,
                                              const float timeline_frame_start,
                                              const float timeline_frame_end);

void final_image_cache_clear(Scene *scene);
void final_image_cache_destroy(Scene *scene);

bool final_image_cache_evict(Scene *scene);

size_t final_image_cache_get_image_count(const Scene *scene);

}  // namespace blender::seq
