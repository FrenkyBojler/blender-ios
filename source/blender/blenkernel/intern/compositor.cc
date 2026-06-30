/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>
#include <string>

#include <fmt/format.h>

#include "BLI_enum_flags.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.hh"
#include "BLI_math_base.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.hh"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_compositor.hh"
#include "BKE_context.hh"
#include "BKE_cryptomatte.hh"
#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"

#include "DEG_depsgraph_build.hh"

#include "WM_api.hh"

#include "DNA_layer_types.h"
#include "DNA_node_types.h"
#include "DNA_object_enums.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "IMB_imbuf.hh"

#include "NOD_dependencies.hh"

namespace blender::bke::compositor {

/* --------------------------------------------------------------------
 * Cache.
 */

Cache::~Cache()
{
  this->clear_frames();
}

const ImBuf *Cache::get_frame(const int frame_number, const int view_identifier)
{
  std::scoped_lock lock{frames_mutex_};
  return this->frames_.lookup_try(FrameKey(frame_number, view_identifier)).value_or(nullptr);
}

void Cache::add_frame(const int frame_number, const int view_identifier, ImBuf *image_buffer)
{
  std::scoped_lock lock{frames_mutex_};
  /* First evict frames if needed to maintain the memory cache limit. In almost all cases, the
   * while loop will run exactly once, since the images in the cache will almost always have the
   * same size, so one goes out, one goes in. So we needn't worry about performance. */
  const int64_t cache_limit = size_t(U.memcachelimit) * 1024 * 1024;
  const int64_t image_size = IMB_get_size_in_memory(image_buffer);
  while (!this->frames_.is_empty() && this->size() + image_size > cache_limit) {
    this->evict_frame(frame_number);
  }

  this->frames_.add_new(FrameKey(frame_number, view_identifier), image_buffer);
}

void Cache::clear_frames()
{
  std::scoped_lock lock{frames_mutex_};
  for (ImBuf *image_buffer : this->frames_.values()) {
    IMB_freeImBuf(image_buffer);
  }
  this->frames_.clear();
}

Vector<IndexRange> Cache::compute_frame_ranges()
{
  /* Compute a sorted vector of all cached frames. */
  VectorSet<int> frame_numbers_set;
  {
    std::scoped_lock lock{frames_mutex_};
    frame_numbers_set.reserve(this->frames_.size());
    for (const FrameKey &key : this->frames_.keys()) {
      frame_numbers_set.add(key.frame_number);
    }
  }
  Vector<int> frame_numbers = frame_numbers_set.extract_vector();
  std::ranges::sort(frame_numbers);

  Vector<IndexRange> frame_ranges;
  for (const int frame : frame_numbers) {
    /* We start a new range by appending a singleton range of the current frame, either because
     * this is the first range or because the last range will not be contiguous with the current
     * frame. */
    if (frame_ranges.is_empty() || frame - frame_ranges.last().last() > 1) {
      frame_ranges.append(IndexRange(frame, 1));
    }
    else {
      /* Otherwise, the frame is contiguous with the last range, so we just grow its size by 1. */
      frame_ranges.last() = IndexRange(frame_ranges.last().start(),
                                       frame_ranges.last().size() + 1);
    }
  }

  return frame_ranges;
}

void Cache::evict_frame(const int current_frame_number)
{
  if (this->frames_.is_empty()) {
    return;
  }

  /* Find the keys with the maximum and minimum frame numbers. */
  FrameKey minimum_key = FrameKey(std::numeric_limits<int>::max());
  FrameKey maximum_key = FrameKey(std::numeric_limits<int>::lowest());
  for (const FrameKey &key : this->frames_.keys()) {
    if (key.frame_number < minimum_key.frame_number) {
      minimum_key = key;
    }
    if (key.frame_number > maximum_key.frame_number) {
      maximum_key = key;
    }
  }

  /* Prioritize evicting frames that are behind the current frame and are furthest from it. */
  if (minimum_key.frame_number < current_frame_number) {
    IMB_freeImBuf(this->frames_.pop(minimum_key));
    return;
  }

  /* Otherwise, evict the frame that is after the current frame and is furthest from it. */
  IMB_freeImBuf(this->frames_.pop(maximum_key));
}

int64_t Cache::size()
{
  int64_t size = 0;
  for (ImBuf *image_buffer : this->frames_.values()) {
    size += IMB_get_size_in_memory(image_buffer);
  }
  return size;
}

/* --------------------------------------------------------------------
 * Scene Compositor Modifiers.
 */

bool has_any_enabled_modifier(const Scene &scene, const ExecutionMode mode)
{
  for (SceneCompositorModifier &modifier : scene.compositor_modifiers) {
    if (is_modifier_enabled(modifier, mode)) {
      return true;
    }
  }

  return false;
}

SceneCompositorModifier *get_modifier(const Scene *scene, const char *name)
{
  return static_cast<SceneCompositorModifier *>(BLI_findstring(
      &(scene->compositor_modifiers), name, offsetof(SceneCompositorModifier, name)));
}

SceneCompositorModifier *get_active_modifier(const Scene *scene)
{
  for (SceneCompositorModifier &modifier : scene->compositor_modifiers) {
    if (flag_is_set(modifier.flags, SceneCompositorModifierFlags::IsActive)) {
      return &modifier;
    }
  }

  return nullptr;
}

bool is_modifier_enabled(const SceneCompositorModifier &modifier, const ExecutionMode mode)
{
  if (!modifier.node_group) {
    return false;
  }

  switch (mode) {
    case ExecutionMode::Render:
      return flag_is_set(modifier.flags, SceneCompositorModifierFlags::EnableForRender);
    case ExecutionMode::Preview:
      return flag_is_set(modifier.flags, SceneCompositorModifierFlags::EnableForPreview);
  }

  BLI_assert_unreachable();
  return false;
}

void set_active_modifier(const Scene *scene, SceneCompositorModifier *modifier)
{
  for (SceneCompositorModifier &other_modifier : scene->compositor_modifiers) {
    other_modifier.flags &= ~SceneCompositorModifierFlags::IsActive;
  }

  /* Activate the active state of the modifier. */
  modifier->flags |= SceneCompositorModifierFlags::IsActive;
}

void rename_modifier(Scene *scene,
                     SceneCompositorModifier *modifier,
                     const char *new_name,
                     const bool update_animation_data)
{
  std::string old_name = modifier->name;
  STRNCPY_UTF8(modifier->name, new_name);
  BLI_uniquename(&scene->compositor_modifiers,
                 modifier,
                 CTX_DATA_(BLT_I18NCONTEXT_ID_SCENE, "Compositor Modifier"),
                 '.',
                 offsetof(SceneCompositorModifier, name),
                 sizeof(modifier->name));

  if (!update_animation_data) {
    return;
  }

  /* Fix all the animation data which may link to this. */
  AnimData *animation_data = BKE_animdata_from_id(&scene->id);
  if (animation_data) {
    BKE_animdata_fix_paths_rename(&scene->id,
                                  animation_data,
                                  nullptr,
                                  "compositor_modifiers",
                                  old_name.c_str(),
                                  modifier->name,
                                  0,
                                  0,
                                  /*verify_paths=*/true,
                                  /*infix_is_name=*/true);
  }
}

SceneCompositorModifier *new_modifier(Scene *scene, const char *name)
{
  SceneCompositorModifier *modifier = MEM_new<SceneCompositorModifier>(
      "Scene Compositor Modifier");
  rename_modifier(scene, modifier, name, false);
  BLI_addtail(&scene->compositor_modifiers, modifier);
  set_active_modifier(scene, modifier);
  return modifier;
}

SceneCompositorModifier *copy_modifier(Scene *scene, SceneCompositorModifier *source_modifier)
{
  SceneCompositorModifier *new_modifier = MEM_dupalloc(source_modifier);
  if (source_modifier->node_group) {
    id_us_plus(&new_modifier->node_group->id);
  }
  BLI_addtail(&scene->compositor_modifiers, new_modifier);
  rename_modifier(scene, new_modifier, source_modifier->name, false);
  set_active_modifier(scene, new_modifier);
  return new_modifier;
}

void remove_modifier(Scene *scene, SceneCompositorModifier *modifier)
{
  if (modifier->node_group) {
    id_us_min(&modifier->node_group->id);
  }
  BLI_remlink(&scene->compositor_modifiers, modifier);
  MEM_delete(modifier);
  if (!scene->compositor_modifiers.is_empty()) {
    set_active_modifier(scene, &*scene->compositor_modifiers.begin());
  }
}

void clear_modifiers(Scene *scene)
{
  for (SceneCompositorModifier &modifier : scene->compositor_modifiers) {
    MEM_delete(&modifier);
  }
  BLI_listbase_clear(&scene->compositor_modifiers);
}

/* --------------------------------------------------------------------
 * Query.
 */

/* Adds the pass names of the passes used by the given Render Layer node to the given used passes.
 * This essentially adds the pass names of the outputs that are logically linked. */
static void add_passes_used_by_render_layer_node(const bNode *node, Set<std::string> &used_passes)
{
  for (const bNodeSocket *output : node->output_sockets()) {
    if (output->is_logically_linked()) {
      /* The combined pass is aliased as Image and Alpha is generated by the node based on the
       * combined pass. */
      if (output->identifier == StringRef("Image") || output->identifier == StringRef("Alpha")) {
        used_passes.add(RE_PASSNAME_COMBINED);
      }
      else {
        used_passes.add(output->identifier);
      }
    }
  }
}

// TODO: Update for compositor modifiers.
/* Adds the pass names of the passes used by the given Group Input node to the given used passes.
 * The Group Input node only uses the combined pass for the first input, while the rest are
 * ignored. */
static void add_passes_used_by_group_input_node(const bNode *node, Set<std::string> &used_passes)
{
  /* Only the virtual socket exists, so no pass is used. */
  if (node->output_sockets().size() == 1) {
    return;
  }

  if (!node->output_sockets()[0]->is_logically_linked()) {
    return;
  }

  used_passes.add(RE_PASSNAME_COMBINED);
}

/* Adds the pass names of all Cryptomatte layers needed by the given node to the given used passes.
 * Only passes in the given viewer layers are added. */
static void add_passes_used_by_cryptomatte_node(const bNode *node,
                                                const ViewLayer *view_layer,
                                                Set<std::string> &used_passes)
{
  if (node->custom1 != CMP_NODE_CRYPTOMATTE_SOURCE_RENDER) {
    return;
  }

  Scene *scene = reinterpret_cast<Scene *>(node->id);
  if (!scene) {
    return;
  }

  cryptomatte::CryptomatteSessionPtr session = cryptomatte::CryptomatteSessionPtr(
      BKE_cryptomatte_init_from_scene(scene, false));

  const Vector<std::string> &layer_names = cryptomatte::BKE_cryptomatte_layer_names_get(*session);
  if (layer_names.is_empty()) {
    return;
  }

  /* If the stored layer name doesn't corresponds to an existing Cryptomatte layer, fall back to
   * the name of the first layer. */
  const NodeCryptomatte *data = static_cast<NodeCryptomatte *>(node->storage);
  const std::string layer_name = layer_names.contains(data->layer_name) ? data->layer_name :
                                                                          layer_names[0];

  /* Does not use passes from the given view layer, so no need to add anything. */
  if (!StringRef(layer_name).startswith(view_layer->name)) {
    return;
  }

  /* Find out which type of Cryptomatte layers the node needs. Also ensure the type is enabled in
   * the view layer, because the node can use one of the types as a placeholder. */
  const char *cryptomatte_type_name = nullptr;
  if (StringRef(layer_name).endswith(RE_PASSNAME_CRYPTOMATTE_OBJECT)) {
    if (view_layer->cryptomatte_flag & VIEW_LAYER_CRYPTOMATTE_OBJECT) {
      cryptomatte_type_name = RE_PASSNAME_CRYPTOMATTE_OBJECT;
    }
  }
  else if (StringRef(layer_name).endswith(RE_PASSNAME_CRYPTOMATTE_ASSET)) {
    if (view_layer->cryptomatte_flag & VIEW_LAYER_CRYPTOMATTE_ASSET) {
      cryptomatte_type_name = RE_PASSNAME_CRYPTOMATTE_ASSET;
    }
  }
  else if (StringRef(layer_name).endswith(RE_PASSNAME_CRYPTOMATTE_MATERIAL)) {
    if (view_layer->cryptomatte_flag & VIEW_LAYER_CRYPTOMATTE_MATERIAL) {
      cryptomatte_type_name = RE_PASSNAME_CRYPTOMATTE_MATERIAL;
    }
  }

  if (!cryptomatte_type_name) {
    return;
  }

  /* Each layer stores two ranks/levels, so do ceiling division by two. */
  const int cryptomatte_layers_count = int(math::ceil(view_layer->cryptomatte_levels / 2.0f));
  for (const int i : IndexRange(cryptomatte_layers_count)) {
    used_passes.add(fmt::format("{}{:02}", cryptomatte_type_name, i));
  }
}

/* Adds the pass names of the passes used by the given compositor node tree to the given used
 * passes. This is called recursively for node groups. */
static void add_used_passes_recursive(const bNodeTree *node_tree,
                                      const ViewLayer *view_layer,
                                      const bool is_root_tree,
                                      Set<const bNodeTree *> &node_trees_already_searched,
                                      Set<std::string> &used_passes)
{
  if (node_tree == nullptr) {
    return;
  }

  node_tree->ensure_topology_cache();
  for (const bNode *node : node_tree->all_nodes()) {
    if (node->is_muted()) {
      continue;
    }

    switch (node->type_legacy) {
      case NODE_GROUP:
      case NODE_CUSTOM_GROUP: {
        const bNodeTree *node_group_tree = reinterpret_cast<const bNodeTree *>(node->id);
        if (node_trees_already_searched.add(node_group_tree)) {
          add_used_passes_recursive(
              node_group_tree, view_layer, false, node_trees_already_searched, used_passes);
        }
        break;
      }
      case CMP_NODE_R_LAYERS:
        add_passes_used_by_render_layer_node(node, used_passes);
        break;
      case NODE_GROUP_INPUT:
        if (is_root_tree) {
          add_passes_used_by_group_input_node(node, used_passes);
        }
        break;
      case CMP_NODE_CRYPTOMATTE:
        add_passes_used_by_cryptomatte_node(node, view_layer, used_passes);
        break;
      default:
        break;
    }
  }
}

Set<std::string> get_used_passes(const Scene &scene,
                                 const ViewLayer *view_layer,
                                 const ExecutionMode mode)
{
  Set<std::string> used_passes;
  Set<const bNodeTree *> node_trees_already_searched;
  for (const SceneCompositorModifier &modifier : scene.compositor_modifiers) {
    if (!is_modifier_enabled(modifier, mode)) {
      continue;
    }
    add_used_passes_recursive(
        modifier.node_group, view_layer, true, node_trees_already_searched, used_passes);
  }
  return used_passes;
}

bool is_viewport_compositor_used(const bContext &context)
{
  const Scene *scene = CTX_data_scene(&context);
  if (!has_any_enabled_modifier(*scene, ExecutionMode::Preview)) {
    return false;
  }

  wmWindowManager *window_manager = CTX_wm_manager(&context);
  for (const wmWindow &window : window_manager->windows) {
    const bScreen *screen = WM_window_get_active_screen(&window);
    for (const ScrArea &area : screen->areabase) {
      const SpaceLink &space = *static_cast<const SpaceLink *>(area.spacedata.first);
      if (space.spacetype == SPACE_VIEW3D) {
        const View3D &view_3d = reinterpret_cast<const View3D &>(space);

        if (view_3d.shading.use_compositor == V3D_SHADING_USE_COMPOSITOR_DISABLED) {
          continue;
        }

        if (!(view_3d.shading.type >= OB_MATERIAL)) {
          continue;
        }

        return true;
      }
    }
  }

  return false;
}

/* --------------------------------------------------------------------
 * Depsgraph.
 */

void add_depsgraph_relations(Scene &scene,
                             const bNodeTree &node_group,
                             DepsNodeHandle *compositor_output_depsgraph_node)
{
  nodes::EvalDependencies evaluation_dependencies = nodes::gather_eval_dependencies_recursive(
      node_group);

  for (ID *id : evaluation_dependencies.ids.values()) {
    switch (ID_Type(GS(id->name))) {
      case ID_OB: {
        Object *object = reinterpret_cast<Object *>(id);
        const nodes::EvalDependencies::ObjectDependencyInfo &info =
            evaluation_dependencies.objects_info.lookup_default(object->id.session_uid, {});
        if (info.transform) {
          DEG_add_object_relation(compositor_output_depsgraph_node,
                                  object,
                                  DEG_OB_COMP_TRANSFORM,
                                  "Object Transform -> Compositor");
        }
        if (object->type == OB_CAMERA && info.camera_parameters) {
          DEG_add_object_relation(compositor_output_depsgraph_node,
                                  object,
                                  DEG_OB_COMP_PARAMETERS,
                                  "Camera Parameters -> Compositor");
        }
        break;
      }
      case ID_IM:
        DEG_add_generic_id_relation(compositor_output_depsgraph_node, id, "Image -> Compositor");
        break;
      case ID_TE:
        DEG_add_generic_id_relation(compositor_output_depsgraph_node, id, "Texture -> Compositor");
        break;
      case ID_VF:
        DEG_add_vfont_relation(
            compositor_output_depsgraph_node, reinterpret_cast<VFont *>(id), "Font -> Compositor");
        break;
      default:
        break;
    }
  }

  if (evaluation_dependencies.needs_active_camera) {
    DEG_add_scene_camera_relation(compositor_output_depsgraph_node,
                                  &scene,
                                  DEG_OB_COMP_TRANSFORM,
                                  "Active Camera Transforms -> Compositor");
  }

  /* Active camera is a scene parameter that can change, so we need a relation for that, too. */
  if (evaluation_dependencies.needs_active_camera ||
      evaluation_dependencies.needs_scene_render_params)
  {
    DEG_add_scene_relation(compositor_output_depsgraph_node,
                           &scene,
                           DEG_SCENE_COMP_PARAMETERS,
                           "Active Camera Parameters -> Compositor");
  }

  if (evaluation_dependencies.time_dependent) {
    DEG_add_time_source_relation(compositor_output_depsgraph_node, "Time Source -> Compositor");
  }
}

}  // namespace blender::bke::compositor
