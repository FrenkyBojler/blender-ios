/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <sstream>

#include "BKE_bake_geometry_nodes_modifier.hh"
#include "BKE_collection.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"

#include "BLI_fileops.hh"
#include "BLI_listbase.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"

#include "MOD_nodes.hh"

namespace blender::bke::bake {

void SimulationNodeCache::reset()
{
  std::destroy_at(this);
  new (this) SimulationNodeCache();
}

void BakeNodeCache::reset()
{
  std::destroy_at(this);
  new (this) BakeNodeCache();
}

void NodeBakeCache::reset()
{
  std::destroy_at(this);
  new (this) NodeBakeCache();
}

IndexRange NodeBakeCache::frame_range() const
{
  if (this->frames.is_empty()) {
    return {};
  }
  const int start_frame = this->frames.first()->frame.frame();
  const int end_frame = this->frames.last()->frame.frame();
  return IndexRange::from_begin_end_inclusive(start_frame, end_frame);
}

SimulationNodeCache *ModifierCache::get_simulation_node_cache(const int id)
{
  std::unique_ptr<SimulationNodeCache> *ptr = this->simulation_cache_by_id.lookup_ptr(id);
  return ptr ? (*ptr).get() : nullptr;
}

BakeNodeCache *ModifierCache::get_bake_node_cache(const int id)
{
  std::unique_ptr<BakeNodeCache> *ptr = this->bake_cache_by_id.lookup_ptr(id);
  return ptr ? (*ptr).get() : nullptr;
}

NodeBakeCache *ModifierCache::get_node_bake_cache(const int id)
{
  if (SimulationNodeCache *cache = this->get_simulation_node_cache(id)) {
    return &cache->bake;
  }
  if (BakeNodeCache *cache = this->get_bake_node_cache(id)) {
    return &cache->bake;
  }
  return nullptr;
}

void ModifierCache::reset_cache(const int id)
{
  if (SimulationNodeCache *cache = this->get_simulation_node_cache(id)) {
    cache->reset();
  }
  if (BakeNodeCache *cache = this->get_bake_node_cache(id)) {
    cache->reset();
  }
}

void scene_simulation_states_reset(Scene &scene)
{
  FOREACH_SCENE_OBJECT_BEGIN (&scene, ob) {
    for (ModifierData &md : ob->modifiers) {
      if (md.type != eModifierType_Nodes) {
        continue;
      }
      NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(&md);
      if (!nmd->runtime->cache) {
        continue;
      }
      for (auto item : nmd->runtime->cache->simulation_cache_by_id.items()) {
        item.value->reset();
      }
    }
  }
  FOREACH_SCENE_OBJECT_END;
}

std::optional<std::string> get_modifier_bake_path(const Main &bmain,
                                                  const Object &object,
                                                  const NodesModifierData &nmd)
{
  if (StringRef(nmd.bake_directory).is_empty()) {
    return std::nullopt;
  }
  if (!BLI_path_is_rel(nmd.bake_directory)) {
    return nmd.bake_directory;
  }
  const char *base_path = ID_BLEND_PATH(&bmain, &object.id);
  if (StringRef(base_path).is_empty()) {
    return std::nullopt;
  }
  char absolute_bake_dir[FILE_MAX];
  STRNCPY(absolute_bake_dir, nmd.bake_directory);
  BLI_path_abs(absolute_bake_dir, base_path);
  return absolute_bake_dir;
}

std::optional<NodesModifierBakeTarget> get_node_bake_target(const Object & /*object*/,
                                                            const NodesModifierData &nmd,
                                                            int node_id)
{
  const NodesModifierBake *bake = nmd.find_bake(node_id);
  if (!bake) {
    return std::nullopt;
  }
  if (bake->bake_target != NODES_MODIFIER_BAKE_TARGET_INHERIT) {
    return NodesModifierBakeTarget(bake->bake_target);
  }
  if (nmd.bake_target != NODES_MODIFIER_BAKE_TARGET_INHERIT) {
    return NodesModifierBakeTarget(nmd.bake_target);
  }
  return NODES_MODIFIER_BAKE_TARGET_PACKED;
}

std::optional<bake::BakePath> get_node_bake_path(const Main &bmain,
                                                 const Object &object,
                                                 const NodesModifierData &nmd,
                                                 int node_id)
{
  const NodesModifierBake *bake = nmd.find_bake(node_id);
  if (bake == nullptr) {
    return std::nullopt;
  }
  if (bake->flag & NODES_MODIFIER_BAKE_CUSTOM_PATH) {
    if (StringRef(bake->directory).is_empty()) {
      return std::nullopt;
    }
    if (!BLI_path_is_rel(bake->directory)) {
      return BakePath::from_single_root(bake->directory);
    }
    const char *base_path = ID_BLEND_PATH(&bmain, &object.id);
    if (StringRef(base_path).is_empty()) {
      return std::nullopt;
    }
    char absolute_bake_dir[FILE_MAX];
    STRNCPY(absolute_bake_dir, bake->directory);
    BLI_path_abs(absolute_bake_dir, base_path);
    return bake::BakePath::from_single_root(absolute_bake_dir);
  }
  const std::optional<std::string> modifier_bake_path = get_modifier_bake_path(bmain, object, nmd);
  if (!modifier_bake_path) {
    return std::nullopt;
  }
  char bake_dir[FILE_MAX];
  BLI_path_join(
      bake_dir, sizeof(bake_dir), modifier_bake_path->c_str(), std::to_string(node_id).c_str());
  return bake::BakePath::from_single_root(bake_dir);
}

static IndexRange fix_frame_range(const int start, const int end)
{
  const int num_frames = std::max(1, end - start + 1);
  return IndexRange(start, num_frames);
}

static void grow_frame_range(const SubFrame frame,
                             std::optional<SubFrame> &r_min_frame,
                             std::optional<SubFrame> &r_max_frame)
{
  if (!r_min_frame || frame < *r_min_frame) {
    r_min_frame = frame;
  }
  if (!r_max_frame || *r_max_frame < frame) {
    r_max_frame = frame;
  }
}

static std::optional<IndexRange> frame_range_from_bounds(const std::optional<SubFrame> min_frame,
                                                         const std::optional<SubFrame> max_frame)
{
  if (!min_frame || !max_frame) {
    return std::nullopt;
  }
  return IndexRange::from_begin_end_inclusive(min_frame->frame(), max_frame->frame());
}

static std::optional<IndexRange> find_baked_frame_range_in_meta_dir(const StringRefNull meta_dir)
{
  if (!BLI_is_dir(meta_dir.c_str())) {
    return std::nullopt;
  }

  direntry *dir_entries = nullptr;
  const int dir_entries_num = BLI_filelist_dir_contents(meta_dir.c_str(), &dir_entries);
  BLI_SCOPED_DEFER([&]() { BLI_filelist_free(dir_entries, dir_entries_num); });

  std::optional<SubFrame> min_frame;
  std::optional<SubFrame> max_frame;
  for (const int i : IndexRange(dir_entries_num)) {
    const direntry &dir_entry = dir_entries[i];
    const StringRefNull dir_entry_path = dir_entry.path;
    if (!dir_entry_path.endswith(".json")) {
      continue;
    }
    const std::optional<SubFrame> frame = bake::file_name_to_frame(dir_entry.relname);
    if (!frame) {
      continue;
    }
    grow_frame_range(*frame, min_frame, max_frame);
  }

  return frame_range_from_bounds(min_frame, max_frame);
}

std::optional<IndexRange> get_node_bake_frame_range(const Scene &scene,
                                                    const Object & /*object*/,
                                                    const NodesModifierData &nmd,
                                                    int node_id)
{
  const NodesModifierBake *bake = nmd.find_bake(node_id);
  if (bake == nullptr) {
    return std::nullopt;
  }
  if (bake->flag & NODES_MODIFIER_BAKE_CUSTOM_SIMULATION_FRAME_RANGE) {
    return fix_frame_range(bake->frame_start, bake->frame_end);
  }
  if (scene.flag & SCE_CUSTOM_SIMULATION_RANGE) {
    return fix_frame_range(scene.simulation_frame_start, scene.simulation_frame_end);
  }
  return fix_frame_range(scene.r.sfra, scene.r.efra);
}

std::optional<IndexRange> get_node_baked_frame_range(const Main &bmain,
                                                     const Object &object,
                                                     const NodesModifierData &nmd,
                                                     const int node_id)
{
  const NodesModifierBake *bake = nmd.find_bake(node_id);
  if (bake == nullptr) {
    return std::nullopt;
  }
  if (bake->packed) {
    if (bake->packed->meta_files_num == 0) {
      return std::nullopt;
    }
    std::optional<SubFrame> min_frame;
    std::optional<SubFrame> max_frame;
    for (const NodesModifierBakeFile &meta_file :
         Span{bake->packed->meta_files, bake->packed->meta_files_num})
    {
      const std::optional<SubFrame> frame = bake::file_name_to_frame(meta_file.name);
      if (!frame) {
        return std::nullopt;
      }
      grow_frame_range(*frame, min_frame, max_frame);
    }
    return frame_range_from_bounds(min_frame, max_frame);
  }

  const std::optional<BakePath> bake_path = get_node_bake_path(bmain, object, nmd, node_id);
  if (!bake_path) {
    return std::nullopt;
  }
  return find_baked_frame_range_in_meta_dir(bake_path->meta_dir);
}

/**
 * Turn the name into something that can be used as file name. It does not necessarily have to be
 * human readable, but it can help if it is at least partially readable.
 */
static std::string escape_name(const StringRef name)
{
  std::stringstream ss;
  for (const char c : name) {
    /* Only some letters allowed. Digits are not because they could lead to name collisions. */
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')) {
      ss << c;
    }
    else {
      ss << int(c);
    }
  }
  return ss.str();
}

static std::string get_blend_file_name(const Main &bmain)
{
  const StringRefNull blend_file_path = BKE_main_blendfile_path(&bmain);
  char blend_name[FILE_MAX];

  BLI_path_split_file_part(blend_file_path.c_str(), blend_name, sizeof(blend_name));
  const int64_t type_start_index = StringRef(blend_name).rfind(".");
  if (type_start_index == StringRef::not_found) {
    return "";
  }
  blend_name[type_start_index] = '\0';
  return "blendcache_" + StringRef(blend_name);
}

static std::string get_modifier_directory_name(const Object &object, const ModifierData &md)
{
  const std::string object_name_escaped = escape_name(object.id.name + 2);
  const std::string modifier_name_escaped = escape_name(md.name);
  return object_name_escaped + "_" + modifier_name_escaped;
}

std::string get_default_modifier_bake_directory(const Main &bmain,
                                                const Object &object,
                                                const NodesModifierData &nmd)
{
  char dir[FILE_MAX];
  /* Make path that's relative to the .blend file. */
  BLI_path_join(dir,
                sizeof(dir),
                "//",
                get_blend_file_name(bmain).c_str(),
                get_modifier_directory_name(object, nmd.modifier).c_str());
  return dir;
}

std::string get_default_node_bake_directory(const Main &bmain,
                                            const Object &object,
                                            const NodesModifierData &nmd,
                                            int node_id)
{
  char dir[FILE_MAX];
  BLI_path_join(dir,
                sizeof(dir),
                "//",
                get_blend_file_name(bmain).c_str(),
                get_modifier_directory_name(object, nmd.modifier).c_str(),
                std::to_string(node_id).c_str());
  return dir;
}

}  // namespace blender::bke::bake
