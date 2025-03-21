/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

struct Scene;
struct SeqRenderData;
struct Strip;

namespace blender::seq {

/**
 * Start or resume prefetching.
 */
void prefetch_start(const RenderData *context, float timeline_frame);
void prefetch_free(Scene *scene);
bool prefetch_job_is_running(Scene *scene);
void prefetch_get_time_range(Scene *scene, int *r_start, int *r_end);
/**
 * For cache context swapping.
 */
RenderData *prefetch_get_original_context(const RenderData *context);
/**
 * For cache context swapping.
 */
Strip *prefetch_get_original_sequence(Strip *strip, Scene *scene);

}  // namespace blender::seq
