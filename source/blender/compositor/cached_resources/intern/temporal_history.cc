/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "COM_temporal_history.hh"

#include "COM_context.hh"

#include "BLI_assert.h"

#include "BKE_scene_runtime.hh"

#include "MEM_guardedalloc.h"

namespace blender::compositor {

struct TemporalHistoryStore {
  Map<TemporalHistoryKey, std::unique_ptr<TemporalHistory>> map;
};

static const Scene &get_orig_scene(const Scene &scene)
{
  if (scene.id.orig_id != nullptr) {
    return *reinterpret_cast<const Scene *>(scene.id.orig_id);
  }
  return scene;
}

static TemporalHistoryStore &get_store_for_scene(const Scene &scene)
{
  using namespace blender::bke;

  const Scene &orig_scene = get_orig_scene(scene);

  SceneRuntime *runtime = orig_scene.runtime;
  BLI_assert(runtime != nullptr);

  CompositorRuntime &comp_runtime = runtime->compositor;

  if (!comp_runtime.temporal_state) {
    auto *store = MEM_new<TemporalHistoryStore>(__func__);
    comp_runtime.temporal_state = store;
    comp_runtime.temporal_state_free_fn = [](void *state) {
      MEM_delete(static_cast<TemporalHistoryStore *>(state));
    };
  }

  TemporalHistoryStore &store = *static_cast<TemporalHistoryStore *>(comp_runtime.temporal_state);

  return store;
}

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
  /* Intentionally left empty.
   *
   * Temporal history is stored per scene in SceneRuntime::CompositorRuntime::temporal_state and is
   * designed to persist across compositor evaluations (similar to a simulation zone). It is not
   * cleared by the per-evaluation cache reset. */
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
  const Scene &orig_scene = get_orig_scene(scene);

  TemporalHistoryKey key;
  key.scene = &orig_scene;
  key.tree = &tree;
  key.node = &bnode;

  TemporalHistoryStore &store = get_store_for_scene(orig_scene);
  auto &map = store.map;

  auto &history_ptr = map.lookup_or_add_cb(key, [&]() {
    return std::make_unique<TemporalHistory>();
  });

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
