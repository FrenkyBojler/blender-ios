/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "COM_temporal_history.hh"

#include "COM_context.hh"

namespace blender::compositor {

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
  /* First, delete all resources that are no longer needed. */
  map_.remove_if([](auto item) { return !item.value->needed; });

  /* Second, reset the needed status of the remaining resources to false to ready them to track
   * their needed status for the next evaluation. */
  for (auto &value : map_.values()) {
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

  auto &history_ptr = map_.lookup_or_add_cb(
      key, []() { return std::make_unique<TemporalHistory>(); });

  TemporalHistory &history = *history_ptr;
  HistoryEntry &entry = history.entry;

  const bool size_changed = (entry.size != size);
  const bool frame_jump = (entry.last_frame != 0 &&
                           (current_frame < entry.last_frame ||
                            current_frame > entry.last_frame + 1));
  const bool capacity_changed = (entry.capacity_frames != history_frames);

  r_has_history = (!size_changed && !frame_jump && !capacity_changed && entry.stored_frames > 0);

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
