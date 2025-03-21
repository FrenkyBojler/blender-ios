/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

struct ImBuf;
struct LinkNode;
struct ListBase;
struct Mask;
struct Scene;
struct SeqEffectHandle;
struct RenderData;
struct Strip;

namespace blender::seq {

/* mutable state for sequencer */
struct RenderState {
  LinkNode *scene_parents = nullptr;
};

/* Strip corner coordinates in screen pixel space. Note that they might not be
 * axis aligned when rotation is present. */
struct StripScreenQuad {
  float2 v0, v1, v2, v3;

  bool is_empty() const
  {
    return v0 == v1 && v2 == v3 && v0 == v2;
  }
};

ImBuf *render_seqbase(const RenderData *context,
                      float timeline_frame,
                      int chan_shown,
                      ListBase *channels,
                      ListBase *seqbasep);
void imbuf_to_sequencer_space(const Scene *scene, ImBuf *ibuf, bool make_float);
blender::Vector<Strip *> shown_strips_get(
    const Scene *scene, ListBase *channels, ListBase *seqbase, int timeline_frame, int chanshown);
ImBuf *render_strip(const RenderData *context,
                    RenderState *state,
                    Strip *strip,
                    float timeline_frame);

/* Renders Mask into an image suitable for sequencer:
 * RGB channels contain mask intensity; alpha channel is opaque. */
ImBuf *render_mask(const RenderData *context, Mask *mask, float frame_index, bool make_float);
void imbuf_assign_spaces(const Scene *scene, ImBuf *ibuf);

StripScreenQuad strip_screen_quad_get(const RenderData *context, const Strip *strip);

}  // namespace blender::seq
