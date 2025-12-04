/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "COM_cached_resource.hh"

namespace blender::compositor {

class Context;

struct HistoryEntry {
  int last_frame = 0;
  int stored_frames = 0;
  int capacity_frames = 0;
  int2 size = int2(0);
  blender::Array<float4> buffer;
  blender::Array<float4> motion_buffer;
  int head = -1;
};

class TemporalHistoryKey {
 public:
  const Scene *scene = nullptr;
  const bNodeTree *tree = nullptr;
  const bNode *node = nullptr;

  uint64_t hash() const;
};

bool operator==(const TemporalHistoryKey &a, const TemporalHistoryKey &b);

class TemporalHistory : public CachedResource {
 public:
  HistoryEntry entry;
};

class TemporalHistoryContainer : public CachedResourceContainer {
 private:
  Map<TemporalHistoryKey, std::unique_ptr<TemporalHistory>> map_;

 public:
  void reset() override;

  HistoryEntry &get(Context &context,
                    const Scene &scene,
                    const bNodeTree &tree,
                    const bNode &bnode,
                    const int2 &size,
                    int current_frame,
                    int history_frames,
                    bool &r_has_history);
};

}  // namespace blender::compositor
