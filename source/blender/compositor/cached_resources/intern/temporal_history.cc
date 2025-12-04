/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "COM_temporal_history.hh"

#include "COM_context.hh"

#include <stdio.h>

namespace blender::compositor {

static Map<TemporalHistoryKey, std::unique_ptr<TemporalHistory>> g_temporal_history_map;

uint64_t TemporalHistoryKey::hash() const
{
  return get_default_hash(scene, tree, node);
}

bool operator==(const TemporalHistoryKey &a, const TemporalHistoryKey &b)
{
  return a.scene == b.scene && a.tree == b.tree && a.node == b.node;
}

void TemporalHistoryContainer::reset()
{
  printf("TD_TEMPORAL_HISTORY_RESET: container=%p size_before=%lld\n",
         (void *)this,
         (long long)g_temporal_history_map.size());
  for (auto &value : g_temporal_history_map.values()) {
    value->needed = false;
  }
}

HistoryEntry &TemporalHistoryContainer::get(Context & /*context*/,
                                            const Scene &scene,
                                            const bNodeTree &tree,
                                            const bNode &bnode,
                                            const int2 &size,
                                            int current_frame,
                                            int history_frames,
                                            bool &r_has_history)
{
  TemporalHistoryKey key;
  key.scene = &scene;
  key.tree = &tree;
  key.node = &bnode;

  auto &history_ptr = g_temporal_history_map.lookup_or_add_cb(key, [&]() {
    printf("TD_TEMPORAL_HISTORY_ALLOC: container=%p map_size_before=%lld\n",
           (void *)this,
           (long long)g_temporal_history_map.size());
    return std::make_unique<TemporalHistory>();
  });

  printf("TD_TEMPORAL_HISTORY_MAP_AFTER: container=%p map_size_after=%lld\n",
         (void *)this,
         (long long)g_temporal_history_map.size());

  TemporalHistory &history = *history_ptr;
  HistoryEntry &entry = history.entry;

  printf("TD_TEMPORAL_HISTORY_KEY: scene=%p tree=%p node=%p history=%p size_before=(%d,%d) cap_before=%d stored_before=%d last_before=%d\n",
         (const void *)key.scene,
         (const void *)key.tree,
         (const void *)key.node,
         (const void *)&history,
         entry.size.x,
         entry.size.y,
         entry.capacity_frames,
         entry.stored_frames,
         entry.last_frame);

  const bool size_changed = (entry.size != size);
  const bool frame_jump = (entry.last_frame != 0 &&
                           (current_frame < entry.last_frame ||
                            current_frame > entry.last_frame + 1));
  const bool capacity_changed = (entry.capacity_frames != history_frames);

  r_has_history = (!size_changed && !frame_jump && !capacity_changed && entry.stored_frames > 0);

  printf("TD_TEMPORAL_HISTORY: size=(%d,%d) frame=%d history_frames=%d size_changed=%d frame_jump=%d capacity_changed=%d stored=%d last=%d has_history=%d\n",
         size.x,
         size.y,
         current_frame,
         history_frames,
         int(size_changed),
         int(frame_jump),
         int(capacity_changed),
         entry.stored_frames,
         entry.last_frame,
         int(r_has_history));

  if (size_changed || capacity_changed) {
    entry.size = size;
    entry.capacity_frames = history_frames;
    const int64_t pixel_count = int64_t(size.x) * size.y;
    const int64_t buffer_size = pixel_count * history_frames;
    entry.buffer.reinitialize(buffer_size);
    entry.motion_buffer.reinitialize(buffer_size);
    entry.stored_frames = 0;
    entry.head = -1;
  }

  if (frame_jump) {
    entry.stored_frames = 0;
    entry.head = -1;
  }

  entry.last_frame = current_frame;
  history.needed = true;
  return entry;
}

}  // namespace blender::compositor
