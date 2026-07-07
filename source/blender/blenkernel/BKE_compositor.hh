/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <string>

#include "BLI_compute_context.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_mutex.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

namespace blender {

struct Scene;
struct ViewLayer;
struct ImBuf;
struct bContext;
struct SceneCompositorModifier;
struct DepsNodeHandle;
struct bNodeTree;
struct PointerRNA;

namespace bke::compositor {

/* --------------------------------------------------------------------
 * Cache.
 */

struct Cache {
  struct FrameKey {
    int frame_number = 0;
    int view_identifier = 0;

    uint64_t hash() const
    {
      return get_default_hash(frame_number, view_identifier);
    }

    friend bool operator==(const FrameKey &a, const FrameKey &b) = default;
  };

 private:
  /* A cache of final interactive compositor results across frames. */
  Map<FrameKey, ImBuf *> frames_;
  /* A mutex for accessing frames_. The frames cache is intrinsically thread-safe since all access
   * happen in the same interactive compositor job, except for drawing cache overlays since it can
   * happen simultaneously while the job is running, that's why the mutex is needed. */
  Mutex frames_mutex_;

 public:
  /* Clear all caches. */
  ~Cache();

  /* Get the frame cache corresponding to the given frame number and view. */
  const ImBuf *get_frame(int frame_number, int view_identifier);

  /* Add a new frame cache entry. If the new entry would surpass the memory cache limit, frames
   * will be evicted to make room. */
  void add_frame(int frame_number, int view_identifier, ImBuf *image_buffer);

  /* Clears the frames cache. */
  void clear_frames();

  /* Computes a list of every contiguous segment of cached frames. Can be used to draw which frame
   * ranges are cached. */
  Vector<IndexRange> compute_frame_ranges();

 private:
  /* Delete one entry from the frames cache given the current frame number. If a cached frame exist
   * before the current frame, the furthest one will be removed, otherwise, the furthest cached
   * frame after the current frame will be removed. */
  void evict_frame(int current_frame_number);

  /* Computes the total size of the cache in bytes. */
  int64_t size();
};

/* --------------------------------------------------------------------
 * Scene Compositor Modifiers.
 */

enum class ExecutionMode : uint8_t {
  /* The compositor is executing for a final render. */
  Render,
  /* The compositor is executing for a preview, like the interactive compositor or the viewport
   * compositor. */
  Preview,
};

/* Returns true if the given scene has any enabled modifier for the given execution mode. */
bool has_any_enabled_modifier(const Scene &scene, ExecutionMode mode);

/* Gets the compositor modifier with the given name in the given scene. */
SceneCompositorModifier *get_modifier(const Scene *scene, const char *name);

/* Gets the active compositor modifier in the given scene. */
SceneCompositorModifier *get_active_modifier(const Scene *scene);

/* Returns true if the given modifier is enabled for the given execution mode. */
bool is_modifier_enabled(const SceneCompositorModifier &modifier, ExecutionMode mode);

/* Sets the given compositor modifier in the given scene to be the active one. */
void set_active_modifier(const Scene *scene, SceneCompositorModifier *modifier);

/* Rename the given compositor modifier in the given scene to the given name. Animation data paths
 * may be updated if update_animation_data is true. */
void rename_modifier(Scene *scene,
                     SceneCompositorModifier *modifier,
                     const char *new_name,
                     bool update_animation_data = true);

/* Adds a new compositor modifier of the given name to the given scene. */
SceneCompositorModifier *new_modifier(Scene *scene, const char *name);

/* Copy the given compositor modifier in the given scene. */
SceneCompositorModifier *copy_modifier(Scene *scene, SceneCompositorModifier *source_modifier);

/* Removes the given compositor modifier from the given scene. */
void remove_modifier(Scene *scene, SceneCompositorModifier *modifier);

/* Removes all compositor modifiers from the given scene. */
void clear_modifiers(Scene *scene);

/* Gets the modifier that the given property belongs to. */
const SceneCompositorModifier *get_modifier_from_property(const PointerRNA &property_ptr);

/* Update the system properties of the modifier. Should be call whenever the node group of the
 * modifier changes or the interface of the assigned node group changes. */
void update_modifier_node_group_interface(Scene &scene, SceneCompositorModifier &modifier);

/* --------------------------------------------------------------------
 * Query.
 */

/* Get the set of all passes used by the compositor for the given view layer and execution mode,
 * identified by their pass names. This might be a superset of the passes actually supported by the
 * render engine, in which case, the compositor will return an invalid output and issue a
 * warning. */
Set<std::string> get_used_passes(const Scene &scene,
                                 const ViewLayer *view_layer,
                                 ExecutionMode mode);

/* Checks if the viewport compositor is currently being used. This is similar to
 * DRWContext::is_viewport_compositor_enabled but checks all 3D views. */
bool is_viewport_compositor_used(const bContext &context);

/* --------------------------------------------------------------------
 * Depsgraph.
 */

/* Add the depsgraph relations needed by the given compositor node group in the given scene. A
 * handle for the compositor output depsgraph node is given to be the target of the relation. */
void add_depsgraph_relations(Scene &scene,
                             const bNodeTree &node_group,
                             DepsNodeHandle *compositor_output_depsgraph_node);

/* Computes the hash of the compositor active compute context. The active compute context is the
 * context that the user last interacted with, see root_node_group.active_viewer_key for more
 * information. */
ComputeContextHash compute_active_compute_context_hash(const Scene &scene);
ComputeContextHash compute_active_compute_context_hash(const Scene &scene,
                                                       const bNodeTree &root_node_group);

}  // namespace bke::compositor
}  // namespace blender
