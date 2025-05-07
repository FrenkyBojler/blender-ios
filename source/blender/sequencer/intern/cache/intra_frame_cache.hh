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

ImBuf *intra_frame_cache_get_preprocessed(Scene *scene, const Strip *strip);
ImBuf *intra_frame_cache_get_composite(Scene *scene, const Strip *strip);
void intra_frame_cache_put_preprocessed(Scene *scene, const Strip *strip, ImBuf *image);
void intra_frame_cache_put_composite(Scene *scene, const Strip *strip, ImBuf *image);

void intra_frame_cache_destroy(Scene *scene);

void intra_frame_cache_invalidate(Scene *scene, const Strip *strip);
void intra_frame_cache_invalidate(Scene *scene);

void intra_frame_cache_set_cur_frame(Scene *scene, float frame);

}  // namespace blender::seq
