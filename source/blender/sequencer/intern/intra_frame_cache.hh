/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#pragma once

#include "BLI_map.hh"

struct ImBuf;
struct Strip;
struct Scene;

namespace blender::seq {

struct IntraFrameCache {
  Map<const Strip *, ImBuf *> cache_;
  float timeline_frame_ = -1.0f;

  ~IntraFrameCache()
  {
    clear();
  }

  ImBuf *get(const Strip *strip) const;
  void put(const Strip *strip, ImBuf *image);
  void invalidate(const Strip *strip);
  void clear();
};

void invalidate_intra_frame_cache(Scene *scene, const Strip *strip);

}  // namespace blender::seq
