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

void source_image_cache_put(const RenderData *context,
                            const Strip *strip,
                            float timeline_frame,
                            ImBuf *image);

ImBuf *source_image_cache_get(const RenderData *context, const Strip *strip, float timeline_frame);

void source_image_cache_maintain_capacity(Scene *scene);

void source_image_cache_invalidate_strip(Scene *scene, const Strip *strip);

void source_image_cache_clear(Scene *scene);
void source_image_cache_destroy(Scene *scene);

}  // namespace blender::seq
