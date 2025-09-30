/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <optional>

#include "MEM_guardedalloc.h"

#include "DNA_defaults.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_volume_types.h"

#include "BLI_bounds.hh"
#include "BLI_fileops.h"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "BKE_anim_data.hh"
#include "BKE_bake_data_block_id.hh"
#include "BKE_bpath.hh"
#include "BKE_geometry_set.hh"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_lib_remap.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_packedFile.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_file_cache.hh"
#include "BKE_volume_grid_process.hh"
#include "BKE_volume_openvdb.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

#ifdef WITH_OPENVDB
static CLG_LogRef LOG = {"geom.volume"};
#endif

#define VOLUME_FRAME_NONE INT_MAX

using blender::float3;
using blender::float4x4;
using blender::IndexRange;
using blender::StringRef;
using blender::StringRefNull;
using blender::bke::GVolumeGrid;

#ifdef WITH_OPENVDB
#  include <list>

#  include <openvdb/openvdb.h>
#  include <openvdb/points/PointDataGrid.h>
#  include <openvdb/tools/GridTransformer.h>

/* Volume Grid Vector
 *
 * List of grids contained in a volume datablock. This is runtime-only data,
 * the actual grids are always saved in a VDB file. */

struct VolumeGridVector : public std::list<GVolumeGrid> {
  VolumeGridVector() : metadata(new openvdb::MetaMap())
  {
    filepath[0] = '\0';
  }

  VolumeGridVector(const VolumeGridVector &other)
      : std::list<GVolumeGrid>(other), error_msg(other.error_msg), metadata(other.metadata)
  {
    memcpy(filepath, other.filepath, sizeof(filepath));
  }

  bool is_loaded() const
  {
    return filepath[0] != '\0';
  }

  void clear_all()
  {
    std::list<GVolumeGrid>::clear();
    filepath[0] = '\0';
    error_msg.clear();
    metadata.reset();
  }

  /* Mutex for file loading of grids list. `const` write access to the fields after this must be
   * protected by locking with this mutex. */
  mutable blender::Mutex mutex;
  /* Absolute file path that grids have been loaded from. */
  char filepath[FILE_MAX];
  /* File loading error message. */
  std::string error_msg;
  /* File Metadata. */
  openvdb::MetaMap::Ptr metadata;
};
#endif

/* Module */

void BKE_volumes_init()
{
#ifdef WITH_OPENVDB
  openvdb::initialize();
#endif
}

/* Volume datablock */

static void volume_init_data(ID *id)
{
  Volume *volume = (Volume *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(volume, id));

  MEMCPY_STRUCT_AFTER(volume, DNA_struct_default_get(Volume), id);

  volume->runtime = MEM_new<blender::bke::VolumeRuntime>(__func__);

  BKE_volume_init_grids(volume);

  STRNCPY(volume->velocity_grid, "velocity");
}

static void volume_copy_data(Main * /*bmain*/,
                             std::optional<Library *> /*owner_library*/,
                             ID *id_dst,
                             const ID *id_src,
                             const int /*flag*/)
{
  Volume *volume_dst = (Volume *)id_dst;
  const Volume *volume_src = (const Volume *)id_src;
  volume_dst->runtime = MEM_new<blender::bke::VolumeRuntime>(__func__);

  if (volume_src->packedfile) {
    volume_dst->packedfile = BKE_packedfile_duplicate(volume_src->packedfile);
  }

  volume_dst->mat = (Material **)MEM_dupallocN(volume_src->mat);
#ifdef WITH_OPENVDB
  if (volume_src->runtime->grids) {
    const VolumeGridVector &grids_src = *(volume_src->runtime->grids);
    volume_dst->runtime->grids = MEM_new<VolumeGridVector>(__func__, grids_src);
  }
#endif

  volume_dst->runtime->frame = volume_src->runtime->frame;
  STRNCPY(volume_dst->runtime->velocity_x_grid, volume_src->runtime->velocity_x_grid);
  STRNCPY(volume_dst->runtime->velocity_y_grid, volume_src->runtime->velocity_y_grid);
  STRNCPY(volume_dst->runtime->velocity_z_grid, volume_src->runtime->velocity_z_grid);

  if (volume_src->runtime->bake_materials) {
    volume_dst->runtime->bake_materials = std::make_unique<blender::bke::bake::BakeMaterialsList>(
        *volume_src->runtime->bake_materials);
  }

  volume_dst->batch_cache = nullptr;
}

static void volume_free_data(ID *id)
{
  Volume *volume = (Volume *)id;
  BKE_animdata_free(&volume->id, false);
  BKE_volume_batch_cache_free(volume);
  MEM_SAFE_FREE(volume->mat);
  if (volume->packedfile) {
    BKE_packedfile_free(volume->packedfile);
    volume->packedfile = nullptr;
  }
#ifdef WITH_OPENVDB
  MEM_delete(volume->runtime->grids);
  volume->runtime->grids = nullptr;
  /* Deleting the volume might have made some grids completely unused, so they can be freed. */
  blender::bke::volume_grid::file_cache::unload_unused();
#endif
  MEM_delete(volume->runtime);
}

static void volume_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Volume *volume = (Volume *)id;
  for (int i = 0; i < volume->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, volume->mat[i], IDWALK_CB_USER);
  }
}

static void volume_foreach_cache(ID *id,
                                 IDTypeForeachCacheFunctionCallback function_callback,
                                 void *user_data)
{
  Volume *volume = (Volume *)id;
  IDCacheKey key = {
      /*id_session_uid*/ id->session_uid,
      /*identifier*/ 1,
  };

  function_callback(id, &key, (void **)&volume->runtime->grids, 0, user_data);
}

static void volume_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  Volume *volume = reinterpret_cast<Volume *>(id);

  if (volume->packedfile != nullptr &&
      (bpath_data->flag & BKE_BPATH_FOREACH_PATH_SKIP_PACKED) != 0)
  {
    return;
  }

  BKE_bpath_foreach_path_fixed_process(bpath_data, volume->filepath, sizeof(volume->filepath));
}

static void volume_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Volume *volume = (Volume *)id;
  const bool is_undo = BLO_write_is_undo(writer);

  /* Do not store packed files in case this is a library override ID. */
  if (ID_IS_OVERRIDE_LIBRARY(volume) && !is_undo) {
    volume->packedfile = nullptr;
  }

  /* write LibData */
  BLO_write_id_struct(writer, Volume, id_address, &volume->id);
  BKE_id_blend_write(writer, &volume->id);

  /* direct data */
  BLO_write_pointer_array(writer, volume->totcol, volume->mat);

  BKE_packedfile_blend_write(writer, volume->packedfile);
}

static void volume_blend_read_data(BlendDataReader *reader, ID *id)
{
  Volume *volume = (Volume *)id;
  volume->runtime = MEM_new<blender::bke::VolumeRuntime>(__func__);

  BKE_packedfile_blend_read(reader, &volume->packedfile, volume->filepath);
  volume->runtime->frame = 0;

  /* materials */
  BLO_read_pointer_array(reader, volume->totcol, (void **)&volume->mat);
}

static void volume_blend_read_after_liblink(BlendLibReader * /*reader*/, ID *id)
{
  Volume *volume = reinterpret_cast<Volume *>(id);

  /* Needs to be done *after* cache pointers are restored (call to
   * `foreach_cache`/`blo_cache_storage_entry_restore_in_new`), easier for now to do it in
   * lib_link... */
  BKE_volume_init_grids(volume);
}

IDTypeInfo IDType_ID_VO = {
    /*id_code*/ Volume::id_type,
    /*id_filter*/ FILTER_ID_VO,
    /*dependencies_id_types*/ FILTER_ID_MA,
    /*main_listbase_index*/ INDEX_ID_VO,
    /*struct_size*/ sizeof(Volume),
    /*name*/ "Volume",
    /*name_plural*/ N_("volumes"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_VOLUME,
    /*flags*/ IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    /*asset_type_info*/ nullptr,

    /*init_data*/ volume_init_data,
    /*copy_data*/ volume_copy_data,
    /*free_data*/ volume_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ volume_foreach_id,
    /*foreach_cache*/ volume_foreach_cache,
    /*foreach_path*/ volume_foreach_path,
    /*foreach_working_space_color*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ volume_blend_write,
    /*blend_read_data*/ volume_blend_read_data,
    /*blend_read_after_liblink*/ volume_blend_read_after_liblink,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ nullptr,
};

void BKE_volume_init_grids(Volume *volume)
{
#ifdef WITH_OPENVDB
  if (volume->runtime->grids == nullptr) {
    volume->runtime->grids = MEM_new<VolumeGridVector>(__func__);
  }
#else
  UNUSED_VARS(volume);
#endif
}

Volume *BKE_volume_add(Main *bmain, const char *name)
{
  Volume *volume = BKE_id_new<Volume>(bmain, name);

  return volume;
}

/* Sequence */

static int volume_sequence_frame(const Depsgraph *depsgraph, const Volume *volume)
{
  if (!volume->is_sequence) {
    return 0;
  }

  int path_frame, path_digits;
  if (!(volume->is_sequence && BLI_path_frame_get(volume->filepath, &path_frame, &path_digits))) {
    return 0;
  }

  const int scene_frame = DEG_get_ctime(depsgraph);
  const VolumeSequenceMode mode = (VolumeSequenceMode)volume->sequence_mode;
  const int frame_duration = volume->frame_duration;
  const int frame_start = volume->frame_start;
  const int frame_offset = volume->frame_offset;

  if (frame_duration == 0) {
    return VOLUME_FRAME_NONE;
  }

  int frame = scene_frame - frame_start + 1;

  switch (mode) {
    case VOLUME_SEQUENCE_CLIP: {
      if (frame < 1 || frame > frame_duration) {
        return VOLUME_FRAME_NONE;
      }
      break;
    }
    case VOLUME_SEQUENCE_EXTEND: {
      frame = clamp_i(frame, 1, frame_duration);
      break;
    }
    case VOLUME_SEQUENCE_REPEAT: {
      frame = frame % frame_duration;
      if (frame < 0) {
        frame += frame_duration;
      }
      if (frame == 0) {
        frame = frame_duration;
      }
      break;
    }
    case VOLUME_SEQUENCE_PING_PONG: {
      const int pingpong_duration = frame_duration * 2 - 2;
      frame = frame % pingpong_duration;
      if (frame < 0) {
        frame += pingpong_duration;
      }
      if (frame == 0) {
        frame = pingpong_duration;
      }
      if (frame > frame_duration) {
        frame = frame_duration * 2 - frame;
      }
      break;
    }
  }

  /* Important to apply after, else we can't loop on e.g. frames 100 - 110. */
  frame += frame_offset;

  return frame;
}

#ifdef WITH_OPENVDB
static void volume_filepath_get(const Main *bmain, const Volume *volume, char r_filepath[FILE_MAX])
{
  BLI_strncpy(r_filepath, volume->filepath, FILE_MAX);
  BLI_path_abs(r_filepath, ID_BLEND_PATH(bmain, &volume->id));

  int path_frame, path_digits;
  if (volume->is_sequence && BLI_path_frame_get(r_filepath, &path_frame, &path_digits)) {
    char ext[32];
    BLI_path_frame_strip(r_filepath, ext, sizeof(ext));
    BLI_path_frame(r_filepath, FILE_MAX, volume->runtime->frame, path_digits);
    BLI_path_extension_ensure(r_filepath, FILE_MAX, ext);
  }
}
#endif

/* File Load */

bool BKE_volume_is_loaded(const Volume *volume)
{
#ifdef WITH_OPENVDB
  /* Test if there is a file to load, or if already loaded. */
  return (volume->filepath[0] == '\0' || volume->runtime->grids->is_loaded());
#else
  UNUSED_VARS(volume);
  return true;
#endif
}

bool BKE_volume_set_velocity_grid_by_name(Volume *volume, const StringRef ref_base_name)
{
  const std::string base_name = ref_base_name;

  if (BKE_volume_grid_find(volume, base_name)) {
    STRNCPY(volume->velocity_grid, base_name.c_str());
    volume->runtime->velocity_x_grid[0] = '\0';
    volume->runtime->velocity_y_grid[0] = '\0';
    volume->runtime->velocity_z_grid[0] = '\0';
    return true;
  }

  /* It could be that the velocity grid is split in multiple grids, try with known postfixes. */
  const StringRefNull postfixes[][3] = {{"x", "y", "z"}, {".x", ".y", ".z"}, {"_x", "_y", "_z"}};

  for (const StringRefNull *postfix : postfixes) {
    bool found = true;
    for (int i = 0; i < 3; i++) {
      std::string post_fixed_name = ref_base_name + postfix[i];
      if (!BKE_volume_grid_find(volume, post_fixed_name)) {
        found = false;
        break;
      }
    }

    if (!found) {
      continue;
    }

    /* Save the base name as well. */
    STRNCPY(volume->velocity_grid, base_name.c_str());
    STRNCPY(volume->runtime->velocity_x_grid, (ref_base_name + postfix[0]).c_str());
    STRNCPY(volume->runtime->velocity_y_grid, (ref_base_name + postfix[1]).c_str());
    STRNCPY(volume->runtime->velocity_z_grid, (ref_base_name + postfix[2]).c_str());
    return true;
  }

  /* Reset to avoid potential issues. */
  volume->velocity_grid[0] = '\0';
  volume->runtime->velocity_x_grid[0] = '\0';
  volume->runtime->velocity_y_grid[0] = '\0';
  volume->runtime->velocity_z_grid[0] = '\0';
  return false;
}

bool BKE_volume_load(const Volume *volume, const Main *bmain)
{
#ifdef WITH_OPENVDB
  const VolumeGridVector &const_grids = *volume->runtime->grids;

  if (volume->runtime->frame == VOLUME_FRAME_NONE) {
    /* Skip loading this frame, outside of sequence range. */
    return true;
  }

  if (BKE_volume_is_loaded(volume)) {
    return const_grids.error_msg.empty();
  }

  /* Double-checked lock. */
  std::lock_guard lock(const_grids.mutex);
  if (BKE_volume_is_loaded(volume)) {
    return const_grids.error_msg.empty();
  }

  /* Guarded by the lock, we can continue to access the grid vector,
   * adding error messages or a new grid, etc. */
  VolumeGridVector &grids = const_cast<VolumeGridVector &>(const_grids);

  /* Get absolute file path at current frame. */
  const char *volume_name = volume->id.name + 2;
  char filepath[FILE_MAX];
  volume_filepath_get(bmain, volume, filepath);

  CLOG_INFO(&LOG, "Volume %s: load %s", volume_name, filepath);

  /* Test if file exists. */
  if (!BLI_exists(filepath)) {
    grids.error_msg = BLI_path_basename(filepath) + std::string(" not found");
    CLOG_INFO(&LOG, "Volume %s: %s", volume_name, grids.error_msg.c_str());
    return false;
  }

  blender::bke::volume_grid::file_cache::GridsFromFile grids_from_file =
      blender::bke::volume_grid::file_cache::get_all_grids_from_file(filepath, 0);

  if (!grids_from_file.error_message.empty()) {
    grids.error_msg = grids_from_file.error_message;
    CLOG_INFO(&LOG, "Volume %s: %s", volume_name, grids.error_msg.c_str());
    return false;
  }

  grids.metadata = std::move(grids_from_file.file_meta_data);
  for (GVolumeGrid &volume_grid : grids_from_file.grids) {
    grids.emplace_back(std::move(volume_grid));
  }

  /* Try to detect the velocity grid. */
  const char *common_velocity_names[] = {"velocity", "vel", "v"};
  for (const char *common_velocity_name : common_velocity_names) {
    if (BKE_volume_set_velocity_grid_by_name(const_cast<Volume *>(volume), common_velocity_name)) {
      break;
    }
  }

  STRNCPY(grids.filepath, filepath);

  return grids.error_msg.empty();
#else
  UNUSED_VARS(bmain, volume);
  return true;
#endif
}

void BKE_volume_unload(Volume *volume)
{
#ifdef WITH_OPENVDB
  VolumeGridVector &grids = *volume->runtime->grids;
  if (grids.filepath[0] != '\0') {
    const char *volume_name = volume->id.name + 2;
    CLOG_INFO(&LOG, "Volume %s: unload", volume_name);
    grids.clear_all();
  }
#else
  UNUSED_VARS(volume);
#endif
}

/* File Save */

bool BKE_volume_save(const Volume *volume,
                     const Main *bmain,
                     ReportList *reports,
                     const char *filepath)
{
#ifdef WITH_OPENVDB
  if (!BKE_volume_load(volume, bmain)) {
    BKE_reportf(reports, RPT_ERROR, "Could not load volume for writing");
    return false;
  }

  VolumeGridVector &grids = *volume->runtime->grids;
  openvdb::GridCPtrVec vdb_grids;

  /* Tree users need to be kept alive for as long as the grids may be accessed. */
  blender::Vector<blender::bke::VolumeTreeAccessToken> tree_tokens;

  for (const GVolumeGrid &grid : grids) {
    tree_tokens.append_as();
    vdb_grids.push_back(grid->grid_ptr(tree_tokens.last()));
  }

  try {
    openvdb::io::File file(filepath);
    file.write(vdb_grids, *grids.metadata);
    file.close();
  }
  catch (const openvdb::IoError &e) {
    BKE_reportf(reports, RPT_ERROR, "Could not write volume: %s", e.what());
    return false;
  }
  catch (...) {
    BKE_reportf(reports, RPT_ERROR, "Could not write volume: Unknown error writing VDB file");
    return false;
  }

  return true;
#else
  UNUSED_VARS(volume, bmain, reports, filepath);
  return false;
#endif
}

void BKE_volume_count_memory(const Volume &volume, blender::MemoryCounter &memory)
{
#ifdef WITH_OPENVDB
  if (const VolumeGridVector *grids = volume.runtime->grids) {
    for (const GVolumeGrid &grid : *grids) {
      grid->count_memory(memory);
    }
  }
#else
  UNUSED_VARS(volume, memory);
#endif
}

std::optional<blender::Bounds<blender::float3>> BKE_volume_min_max(const Volume *volume)
{
#ifdef WITH_OPENVDB
  /* TODO: if we know the volume is going to be displayed, it may be good to
   * load it as part of dependency graph evaluation for better threading. We
   * could also share the bounding box computation in the global volume cache. */
  if (BKE_volume_load(const_cast<Volume *>(volume), G.main)) {
    std::optional<blender::Bounds<blender::float3>> result;
    for (const int i : IndexRange(BKE_volume_num_grids(volume))) {
      const blender::bke::VolumeGridData *volume_grid = BKE_volume_grid_get(volume, i);
      blender::bke::VolumeTreeAccessToken tree_token;
      result = blender::bounds::merge(result,
                                      BKE_volume_grid_bounds(volume_grid->grid_ptr(tree_token)));
    }
    return result;
  }
#else
  UNUSED_VARS(volume);
#endif
  return std::nullopt;
}

bool BKE_volume_is_y_up(const Volume *volume)
{
  /* Simple heuristic for common files to open the right way up. */
#ifdef WITH_OPENVDB
  VolumeGridVector &grids = *volume->runtime->grids;
  if (grids.metadata) {
    openvdb::StringMetadata::ConstPtr creator =
        grids.metadata->getMetadata<openvdb::StringMetadata>("creator");
    if (!creator) {
      creator = grids.metadata->getMetadata<openvdb::StringMetadata>("Creator");
    }
    return (creator && creator->str().rfind("Houdini", 0) == 0);
  }
#else
  UNUSED_VARS(volume);
#endif

  return false;
}

bool BKE_volume_is_points_only(const Volume *volume)
{
  int num_grids = BKE_volume_num_grids(volume);
  if (num_grids == 0) {
    return false;
  }

  for (int i = 0; i < num_grids; i++) {
    const blender::bke::VolumeGridData *grid = BKE_volume_grid_get(volume, i);
    if (blender::bke::volume_grid::get_type(*grid) != VOLUME_GRID_POINTS) {
      return false;
    }
  }

  return true;
}

/* Dependency Graph */

static void volume_update_simplify_level(Main *bmain, Volume *volume, const Depsgraph *depsgraph)
{
#ifdef WITH_OPENVDB
  const int simplify_level = BKE_volume_simplify_level(depsgraph);

  /* Replace grids with the new simplify level variants from the cache. */
  if (BKE_volume_load(volume, bmain)) {
    VolumeGridVector &grids = *volume->runtime->grids;
    std::list<GVolumeGrid> new_grids;
    for (const GVolumeGrid &old_grid : grids) {
      GVolumeGrid simple_grid = blender::bke::volume_grid::file_cache::get_grid_from_file(
          grids.filepath, old_grid->name(), simplify_level);
      BLI_assert(simple_grid);
      new_grids.push_back(std::move(simple_grid));
    }
    grids.swap(new_grids);
  }
#else
  UNUSED_VARS(bmain, volume, depsgraph);
#endif
}

static void volume_evaluate_modifiers(Depsgraph *depsgraph,
                                      Scene *scene,
                                      Object *object,
                                      blender::bke::GeometrySet &geometry_set)
{
  /* Modifier evaluation modes. */
  const bool use_render = (DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  const int required_mode = use_render ? eModifierMode_Render : eModifierMode_Realtime;
  ModifierApplyFlag apply_flag = use_render ? MOD_APPLY_RENDER : MOD_APPLY_USECACHE;
  const ModifierEvalContext mectx = {depsgraph, object, apply_flag};

  BKE_modifiers_clear_errors(object);

  /* Get effective list of modifiers to execute. Some effects like shape keys
   * are added as virtual modifiers before the user created modifiers. */
  VirtualModifierData virtual_modifier_data;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(object, &virtual_modifier_data);

  /* Evaluate modifiers. */
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info((ModifierType)md->type);

    if (!BKE_modifier_is_enabled(scene, md, required_mode)) {
      continue;
    }

    blender::bke::ScopedModifierTimer modifier_timer{*md};

    if (mti->modify_geometry_set) {
      mti->modify_geometry_set(md, &mectx, &geometry_set);
    }
  }
}

void BKE_volume_eval_geometry(Depsgraph *depsgraph, Volume *volume)
{
  Main *bmain = DEG_get_bmain(depsgraph);

  /* TODO: can we avoid modifier re-evaluation when frame did not change? */
  int frame = volume_sequence_frame(depsgraph, volume);
  if (frame != volume->runtime->frame) {
    BKE_volume_unload(volume);
    volume->runtime->frame = frame;
  }

  volume_update_simplify_level(bmain, volume, depsgraph);

  /* Flush back to original. */
  if (DEG_is_active(depsgraph)) {
    Volume *volume_orig = DEG_get_original(volume);
    if (volume_orig->runtime->frame != volume->runtime->frame) {
      BKE_volume_unload(volume_orig);
      volume_orig->runtime->frame = volume->runtime->frame;
    }
  }
}

static Volume *take_volume_ownership_from_geometry_set(blender::bke::GeometrySet &geometry_set)
{
  if (!geometry_set.has<blender::bke::VolumeComponent>()) {
    return nullptr;
  }
  auto &volume_component = geometry_set.get_component_for_write<blender::bke::VolumeComponent>();
  Volume *volume = volume_component.release();
  if (volume != nullptr) {
    /* Add back, but only as read-only non-owning component. */
    volume_component.replace(volume, blender::bke::GeometryOwnershipType::ReadOnly);
  }
  else {
    /* The component was empty, we can remove it. */
    geometry_set.remove<blender::bke::VolumeComponent>();
  }
  return volume;
}

void BKE_volume_data_update(Depsgraph *depsgraph, Scene *scene, Object *object)
{
  /* Free any evaluated data and restore original data. */
  BKE_object_free_derived_caches(object);

  /* Evaluate modifiers. */
  Volume *volume = (Volume *)object->data;
  blender::bke::GeometrySet geometry_set;
  geometry_set.replace_volume(volume, blender::bke::GeometryOwnershipType::ReadOnly);
  volume_evaluate_modifiers(depsgraph, scene, object, geometry_set);

  Volume *volume_eval = take_volume_ownership_from_geometry_set(geometry_set);

  /* If the geometry set did not contain a volume, we still create an empty one. */
  if (volume_eval == nullptr) {
    volume_eval = BKE_volume_new_for_eval(volume);
  }

  /* Assign evaluated object. */
  const bool eval_is_owned = (volume != volume_eval);
  BKE_object_eval_assign_data(object, &volume_eval->id, eval_is_owned);
  object->runtime->geometry_set_eval = new blender::bke::GeometrySet(std::move(geometry_set));
}

void BKE_volume_grids_backup_restore(Volume *volume, VolumeGridVector *grids, const char *filepath)
{
#ifdef WITH_OPENVDB
  /* Restore grids after datablock was re-copied from original by depsgraph,
   * we don't want to load them again if possible. */
  BLI_assert(volume->id.tag & ID_TAG_COPIED_ON_EVAL);
  BLI_assert(volume->runtime->grids != nullptr && grids != nullptr);

  if (!grids->is_loaded()) {
    /* No grids loaded in evaluated datablock, nothing lost by discarding. */
    MEM_delete(grids);
  }
  else if (!STREQ(volume->filepath, filepath)) {
    /* Filepath changed, discard grids from evaluated datablock. */
    MEM_delete(grids);
  }
  else {
    /* Keep grids from evaluated datablock. We might still unload them a little
     * later in BKE_volume_eval_geometry if the frame changes. */
    MEM_delete(volume->runtime->grids);
    volume->runtime->grids = grids;
  }
#else
  UNUSED_VARS(volume, grids, filepath);
#endif
}

/* Draw Cache */

void (*BKE_volume_batch_cache_dirty_tag_cb)(Volume *volume, int mode) = nullptr;
void (*BKE_volume_batch_cache_free_cb)(Volume *volume) = nullptr;

void BKE_volume_batch_cache_dirty_tag(Volume *volume, int mode)
{
  if (volume->batch_cache) {
    BKE_volume_batch_cache_dirty_tag_cb(volume, mode);
  }
}

void BKE_volume_batch_cache_free(Volume *volume)
{
  if (volume->batch_cache) {
    BKE_volume_batch_cache_free_cb(volume);
  }
}

/* Grids */

int BKE_volume_num_grids(const Volume *volume)
{
#ifdef WITH_OPENVDB
  return volume->runtime->grids->size();
#else
  UNUSED_VARS(volume);
  return 0;
#endif
}

const char *BKE_volume_grids_error_msg(const Volume *volume)
{
#ifdef WITH_OPENVDB
  return volume->runtime->grids->error_msg.c_str();
#else
  UNUSED_VARS(volume);
  return "";
#endif
}

const char *BKE_volume_grids_frame_filepath(const Volume *volume)
{
#ifdef WITH_OPENVDB
  return volume->runtime->grids->filepath;
#else
  UNUSED_VARS(volume);
  return "";
#endif
}

const blender::bke::VolumeGridData *BKE_volume_grid_get(const Volume *volume, int grid_index)
{
#ifdef WITH_OPENVDB
  const VolumeGridVector &grids = *volume->runtime->grids;
  for (const GVolumeGrid &grid : grids) {
    if (grid_index-- == 0) {
      return &grid.get();
    }
  }
  return nullptr;
#else
  UNUSED_VARS(volume, grid_index);
  return nullptr;
#endif
}

blender::bke::VolumeGridData *BKE_volume_grid_get_for_write(Volume *volume, int grid_index)
{
#ifdef WITH_OPENVDB
  VolumeGridVector &grids = *volume->runtime->grids;
  for (GVolumeGrid &grid_ptr : grids) {
    if (grid_index-- == 0) {
      return &grid_ptr.get_for_write();
    }
  }
  return nullptr;
#else
  UNUSED_VARS(volume, grid_index);
  return nullptr;
#endif
}

const blender::bke::VolumeGridData *BKE_volume_grid_active_get_for_read(const Volume *volume)
{
  const int num_grids = BKE_volume_num_grids(volume);
  if (num_grids == 0) {
    return nullptr;
  }

  const int index = clamp_i(volume->active_grid, 0, num_grids - 1);
  return BKE_volume_grid_get(volume, index);
}

const blender::bke::VolumeGridData *BKE_volume_grid_find(const Volume *volume,
                                                         const StringRef name)
{
  int num_grids = BKE_volume_num_grids(volume);
  for (int i = 0; i < num_grids; i++) {
    const blender::bke::VolumeGridData *grid = BKE_volume_grid_get(volume, i);
    if (blender::bke::volume_grid::get_name(*grid) == name) {
      return grid;
    }
  }

  return nullptr;
}

blender::bke::VolumeGridData *BKE_volume_grid_find_for_write(Volume *volume, const StringRef name)
{
  int num_grids = BKE_volume_num_grids(volume);
  for (int i = 0; i < num_grids; i++) {
    const blender::bke::VolumeGridData *grid = BKE_volume_grid_get(volume, i);
    if (blender::bke::volume_grid::get_name(*grid) == name) {
      return BKE_volume_grid_get_for_write(volume, i);
    }
  }

  return nullptr;
}

/* Grid Tree and Voxels */

/* Volume Editing */

Volume *BKE_volume_new_for_eval(const Volume *volume_src)
{
  Volume *volume_dst = BKE_id_new_nomain<Volume>(nullptr);

  STRNCPY(volume_dst->id.name, volume_src->id.name);
  volume_dst->mat = (Material **)MEM_dupallocN(volume_src->mat);
  volume_dst->totcol = volume_src->totcol;
  volume_dst->render = volume_src->render;
  volume_dst->display = volume_src->display;

  return volume_dst;
}

Volume *BKE_volume_copy_for_eval(const Volume *volume_src)
{
  return reinterpret_cast<Volume *>(
      BKE_id_copy_ex(nullptr, &volume_src->id, nullptr, LIB_ID_COPY_LOCALIZE));
}

#ifdef WITH_OPENVDB
struct CreateGridOp {
  template<typename GridType> typename openvdb::GridBase::Ptr operator()()
  {
    if constexpr (std::is_same_v<GridType, openvdb::points::PointDataGrid>) {
      return {};
    }
    else {
      return GridType::create();
    }
  }
};
#endif

#ifdef WITH_OPENVDB
blender::bke::VolumeGridData *BKE_volume_grid_add_vdb(Volume &volume,
                                                      const StringRef name,
                                                      openvdb::GridBase::Ptr vdb_grid)
{
  VolumeGridVector &grids = *volume.runtime->grids;
  BLI_assert(BKE_volume_grid_find(&volume, name) == nullptr);
  BLI_assert(blender::bke::volume_grid::get_type(*vdb_grid) != VOLUME_GRID_UNKNOWN);

  vdb_grid->setName(name);
  grids.emplace_back(GVolumeGrid(std::move(vdb_grid)));
  return &grids.back().get_for_write();
}

void BKE_volume_metadata_set(Volume &volume, openvdb::MetaMap::Ptr metadata)
{
  volume.runtime->grids->metadata = metadata;
}
#endif

void BKE_volume_grid_remove(Volume *volume, const blender::bke::VolumeGridData *grid)
{
#ifdef WITH_OPENVDB
  VolumeGridVector &grids = *volume->runtime->grids;
  for (VolumeGridVector::iterator it = grids.begin(); it != grids.end(); it++) {
    if (&it->get() == grid) {
      grids.erase(it);
      break;
    }
  }
#else
  UNUSED_VARS(volume, grid);
#endif
}

void BKE_volume_grid_add(Volume *volume, const blender::bke::VolumeGridData &grid)
{
#ifdef WITH_OPENVDB
  VolumeGridVector &grids = *volume->runtime->grids;
  grids.push_back(GVolumeGrid(&grid));
#else
  UNUSED_VARS(volume, grid);
#endif
}

bool BKE_volume_grid_determinant_valid(const double determinant)
{
#ifdef WITH_OPENVDB
  /* Limit taken from openvdb/math/Maps.h. */
  return std::abs(determinant) >= 3.0 * openvdb::math::Tolerance<double>::value();
#else
  UNUSED_VARS(determinant);
  return true;
#endif
}

bool BKE_volume_voxel_size_valid(const float3 &voxel_size)
{
  return BKE_volume_grid_determinant_valid(voxel_size[0] * voxel_size[1] * voxel_size[2]);
}

bool BKE_volume_grid_transform_valid(const float4x4 &transform)
{
  return BKE_volume_grid_determinant_valid(blender::math::determinant(transform));
}

int BKE_volume_simplify_level(const Depsgraph *depsgraph)
{
  if (DEG_get_mode(depsgraph) != DAG_EVAL_RENDER) {
    const Scene *scene = DEG_get_input_scene(depsgraph);
    if (scene->r.mode & R_SIMPLIFY) {
      const float simplify = scene->r.simplify_volumes;
      if (simplify == 0.0f) {
        /* log2 is not defined at 0.0f, so just use some high simplify level. */
        return 16;
      }
      return ceilf(-log2(simplify));
    }
  }
  return 0;
}

float BKE_volume_simplify_factor(const Depsgraph *depsgraph)
{
  if (DEG_get_mode(depsgraph) != DAG_EVAL_RENDER) {
    const Scene *scene = DEG_get_input_scene(depsgraph);
    if (scene->r.mode & R_SIMPLIFY) {
      return scene->r.simplify_volumes;
    }
  }
  return 1.0f;
}

/* OpenVDB Grid Access */

#ifdef WITH_OPENVDB

std::optional<blender::Bounds<float3>> BKE_volume_grid_bounds(openvdb::GridBase::ConstPtr grid)
{
  /* TODO: we can get this from grid metadata in some cases? */
  openvdb::CoordBBox coordbbox;
  if (!grid->baseTree().evalLeafBoundingBox(coordbbox)) {
    return std::nullopt;
  }

  openvdb::BBoxd index_bbox = {
      openvdb::BBoxd(coordbbox.min().asVec3d(), coordbbox.max().asVec3d())};
  /* Add half voxel padding that is expected by volume rendering code. */
  index_bbox.expand(0.5);

  const openvdb::BBoxd bbox = grid->transform().indexToWorld(index_bbox);
  return blender::Bounds<float3>{float3(bbox.min().asPointer()), float3(bbox.max().asPointer())};
}

openvdb::GridBase::ConstPtr BKE_volume_grid_shallow_transform(openvdb::GridBase::ConstPtr grid,
                                                              const blender::float4x4 &transform)
{
  openvdb::math::Transform::Ptr grid_transform = grid->transform().copy();
  grid_transform->postMult(openvdb::Mat4d((float *)transform.ptr()));

  /* Create a transformed grid. The underlying tree is shared. */
  return grid->copyGridReplacingTransform(grid_transform);
}

blender::float4x4 BKE_volume_transform_to_blender(const openvdb::math::Transform &transform)
{
  /* Perspective not supported for now, getAffineMap() will leave out the
   * perspective part of the transform. */
  const openvdb::math::Mat4f matrix = transform.baseMap()->getAffineMap()->getMat4();
  /* Blender column-major and OpenVDB right-multiplication conventions match. */
  float4x4 result;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      result[col][row] = matrix(col, row);
    }
  }
  return result;
}

openvdb::math::Transform BKE_volume_transform_to_openvdb(const blender::float4x4 &transform)
{
  openvdb::math::Mat4f matrix_openvdb;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      matrix_openvdb(col, row) = transform[col][row];
    }
  }
  return openvdb::math::Transform(std::make_shared<openvdb::math::AffineMap>(matrix_openvdb));
}

/* Changing the resolution of a grid. */

/**
 * Returns a grid of the same type as the input, but with more/less resolution. If
 * resolution_factor is 1/2, the resolution on each axis is halved. The transform of the returned
 * grid is adjusted to match the original grid. */
template<typename GridType>
static typename GridType::Ptr create_grid_with_changed_resolution(const GridType &old_grid,
                                                                  const float resolution_factor)
{
  BLI_assert(resolution_factor > 0.0f);

  openvdb::Mat4R xform;
  xform.setToScale(openvdb::Vec3d(resolution_factor));
  openvdb::tools::GridTransformer transformer{xform};

  typename GridType::Ptr new_grid = old_grid.copyWithNewTree();
  transformer.transformGrid<openvdb::tools::BoxSampler>(old_grid, *new_grid);
  new_grid->transform() = old_grid.transform();
  new_grid->transform().preScale(1.0f / resolution_factor);
  new_grid->transform().postTranslate(-new_grid->voxelSize() / 2.0f);
  return new_grid;
}

struct CreateGridWithChangedResolutionOp {
  const openvdb::GridBase &grid;
  const float resolution_factor;

  template<typename GridType> typename openvdb::GridBase::Ptr operator()()
  {
    return create_grid_with_changed_resolution(static_cast<const GridType &>(grid),
                                               resolution_factor);
  }
};

openvdb::GridBase::Ptr BKE_volume_grid_create_with_changed_resolution(
    const VolumeGridType grid_type,
    const openvdb::GridBase &old_grid,
    const float resolution_factor)
{
  CreateGridWithChangedResolutionOp op{old_grid, resolution_factor};
  return BKE_volume_grid_type_operation(grid_type, op);
}

namespace blender::bke {

/**
 * Call #process_leaf_fn on the leaf node if it has a certain minimum number of active voxels. If
 * there are only a few active voxels, gather those in #r_coords for later batch processing.
 */
template<typename LeafNodeT>
static void parallel_grid_topology_tasks_leaf_node(const LeafNodeT &node,
                                                   const ProcessLeafFn process_leaf_fn,
                                                   Vector<openvdb::Coord, 1024> &r_coords)
{
  using NodeMaskT = typename LeafNodeT::NodeMaskType;

  const int on_count = node.onVoxelCount();
  /* This number is somewhat arbitrary. 64 is a 1/8th of the number of voxels in a standard leaf
   * which is 8x8x8. It's a trade-off between benefiting from the better performance of
   * leaf-processing vs. processing more voxels in a batch. */
  const int on_count_threshold = 64;
  if (on_count <= on_count_threshold) {
    /* The leaf contains only a few active voxels. It's beneficial to process them in a batch with
     * active voxels from other leafs. So only gather them here for later processing. */
    for (auto value_iter = node.cbeginValueOn(); value_iter.test(); ++value_iter) {
      const openvdb::Coord coord = value_iter.getCoord();
      r_coords.append(coord);
    }
    return;
  }
  /* Process entire leaf at once. This is especially beneficial when very many of the voxels in
   * the leaf are active. In that case, one can work on the openvdb arrays stored in the leafs
   * directly. */
  const NodeMaskT &value_mask = node.getValueMask();
  const openvdb::CoordBBox bbox = node.getNodeBoundingBox();
  process_leaf_fn(value_mask, bbox, [&](MutableSpan<openvdb::Coord> r_voxels) {
    for (auto value_iter = node.cbeginValueOn(); value_iter.test(); ++value_iter) {
      r_voxels[value_iter.pos()] = value_iter.getCoord();
    }
  });
}

/**
 * Calls the process functions on all the active tiles and voxels within the given internal node.
 */
template<typename InternalNodeT>
static void parallel_grid_topology_tasks_internal_node(const InternalNodeT &node,
                                                       const ProcessLeafFn process_leaf_fn,
                                                       const ProcessVoxelsFn process_voxels_fn,
                                                       const ProcessTilesFn process_tiles_fn)
{
  using ChildNodeT = typename InternalNodeT::ChildNodeType;
  using LeafNodeT = typename InternalNodeT::LeafNodeType;
  using NodeMaskT = typename InternalNodeT::NodeMaskType;
  using UnionT = typename InternalNodeT::UnionType;

  /* Gather the active sub-nodes first, to be able to parallelize over them more easily. */
  const NodeMaskT &child_mask = node.getChildMask();
  const UnionT *table = node.getTable();
  Vector<int, 512> child_indices;
  for (auto child_mask_iter = child_mask.beginOn(); child_mask_iter.test(); ++child_mask_iter) {
    child_indices.append(child_mask_iter.pos());
  }

  threading::parallel_for(child_indices.index_range(), 8, [&](const IndexRange range) {
    /* Voxels collected from potentially multiple leaf nodes to be processed in one batch. This
     * inline buffer size is sufficient to avoid an allocation in all cases (a single standard leaf
     * has 512 voxels). */
    Vector<openvdb::Coord, 1024> gathered_voxels;
    for (const int child_index : child_indices.as_span().slice(range)) {
      const ChildNodeT &child = *table[child_index].getChild();
      if constexpr (std::is_same_v<ChildNodeT, LeafNodeT>) {
        parallel_grid_topology_tasks_leaf_node(child, process_leaf_fn, gathered_voxels);
        /* If enough voxels have been gathered, process them in one batch. */
        if (gathered_voxels.size() >= 512) {
          process_voxels_fn(gathered_voxels);
          gathered_voxels.clear();
        }
      }
      else {
        /* Recurse into lower-level internal nodes. */
        parallel_grid_topology_tasks_internal_node(
            child, process_leaf_fn, process_voxels_fn, process_tiles_fn);
      }
    }
    /* Process any remaining voxels. */
    if (!gathered_voxels.is_empty()) {
      process_voxels_fn(gathered_voxels);
      gathered_voxels.clear();
    }
  });

  /* Process the active tiles within the internal node. Note that these are not processed above
   * already because there only sub-nodes are handled, but tiles are "inlined" into internal nodes.
   * All tiles are first gathered and then processed in one batch. */
  const NodeMaskT &value_mask = node.getValueMask();
  Vector<openvdb::CoordBBox> tile_bboxes;
  for (auto value_mask_iter = value_mask.beginOn(); value_mask_iter.test(); ++value_mask_iter) {
    const openvdb::Index32 index = value_mask_iter.pos();
    const openvdb::Coord tile_origin = node.offsetToGlobalCoord(index);
    const openvdb::CoordBBox tile_bbox = openvdb::CoordBBox::createCube(tile_origin,
                                                                        ChildNodeT::DIM);
    tile_bboxes.append(tile_bbox);
  }
  if (!tile_bboxes.is_empty()) {
    process_tiles_fn(tile_bboxes);
  }
}

/* Call the process functions on all active tiles and voxels in the given tree. */
void parallel_grid_topology_tasks(const openvdb::MaskTree &mask_tree,
                                  const ProcessLeafFn process_leaf_fn,
                                  const ProcessVoxelsFn process_voxels_fn,
                                  const ProcessTilesFn process_tiles_fn)
{
  /* Iterate over the root internal nodes. */
  for (auto root_child_iter = mask_tree.cbeginRootChildren(); root_child_iter.test();
       ++root_child_iter)
  {
    const auto &internal_node = *root_child_iter;
    parallel_grid_topology_tasks_internal_node(
        internal_node, process_leaf_fn, process_voxels_fn, process_tiles_fn);
  }
}

openvdb::GridBase::Ptr create_grid_with_topology(const openvdb::TreeBase &topology,
                                                 const openvdb::math::Transform &transform,
                                                 const VolumeGridType grid_type)
{
  openvdb::GridBase::Ptr grid;
  const VolumeGridType topology_tree_type = bke::volume_grid::get_type(topology);
  BKE_volume_grid_type_to_static_type(topology_tree_type, [&](auto topology_type_tag) {
    using TopologyGridT = typename decltype(topology_type_tag)::type;
    using TopologyTreeT = typename TopologyGridT::TreeType;
    const TopologyTreeT &topology_typed = static_cast<const TopologyTreeT &>(topology);
    BKE_volume_grid_type_to_static_type(grid_type, [&](auto type_tag) {
      using GridT = typename decltype(type_tag)::type;
      using TreeT = typename GridT::TreeType;
      using ValueType = typename TreeT::ValueType;
      const ValueType background{};
      auto tree = std::make_shared<TreeT>(topology_typed, background, openvdb::TopologyCopy());
      grid = openvdb::createGrid(std::move(tree));
      grid->setTransform(transform.copy());
    });
  });
  return grid;
}

void set_grid_values(openvdb::GridBase &grid_base,
                     const GSpan values,
                     const Span<openvdb::Coord> voxels)
{
  bke::to_typed_grid(grid_base, [&](auto &grid) {
    using GridT = std::decay_t<decltype(grid)>;
    using ValueType = typename GridT::ValueType;
    const ValueType *data = static_cast<const ValueType *>(values.data());

    auto accessor = grid.getUnsafeAccessor();
    for (const int64_t i : voxels.index_range()) {
      accessor.setValue(voxels[i], data[i]);
    }
  });
}

void set_tile_values(openvdb::GridBase &grid_base,
                     const GSpan values,
                     const Span<openvdb::CoordBBox> tiles)
{
  bke::to_typed_grid(grid_base, [&](auto &grid) {
    using GridT = typename std::decay_t<decltype(grid)>;
    using TreeT = typename GridT::TreeType;
    using ValueType = typename GridT::ValueType;
    auto &tree = grid.tree();

    const ValueType *computed_values = static_cast<const ValueType *>(values.data());

    const auto set_tile_value = [&](auto &node, const openvdb::Coord &coord_in_tile, auto value) {
      const openvdb::Index n = node.coordToOffset(coord_in_tile);
      BLI_assert(node.isChildMaskOff(n));
      /* TODO: Figure out how to do this without const_cast, although the same is done in
       * `openvdb_ax/openvdb_ax/compiler/VolumeExecutable.cc` which has a similar purpose.
       * It seems like OpenVDB generally allows that, but it does not have a proper public
       * API for this yet. */
      using UnionType = typename std::decay_t<decltype(node)>::UnionType;
      auto *table = const_cast<UnionType *>(node.getTable());
      table[n].setValue(value);
    };

    for (const int i : tiles.index_range()) {
      const openvdb::CoordBBox tile = tiles[i];
      const openvdb::Coord coord_in_tile = tile.min();
      const auto &computed_value = computed_values[i];
      using InternalNode1 = typename TreeT::RootNodeType::ChildNodeType;
      using InternalNode2 = typename InternalNode1::ChildNodeType;
      /* Find the internal node that contains the tile and update the value in there. */
      if (auto *node = tree.template probeNode<InternalNode2>(coord_in_tile)) {
        set_tile_value(*node, coord_in_tile, computed_value);
      }
      else if (auto *node = tree.template probeNode<InternalNode1>(coord_in_tile)) {
        set_tile_value(*node, coord_in_tile, computed_value);
      }
      else {
        BLI_assert_unreachable();
      }
    }
  });
}

void set_mask_leaf_buffer_from_bools(openvdb::BoolGrid &grid,
                                     const Span<bool> values,
                                     const IndexMask &index_mask,
                                     const Span<openvdb::Coord> voxels)
{
  auto accessor = grid.getUnsafeAccessor();
  /* Could probably use int16_t for the iteration index. Double check this. */
  index_mask.foreach_index_optimized<int>([&](const int i) {
    const openvdb::Coord &coord = voxels[i];
    accessor.setValue(coord, values[i]);
  });
}

void set_grid_background(openvdb::GridBase &grid_base, const GPointer value)
{
  bke::to_typed_grid(grid_base, [&](auto &grid) {
    using GridT = std::decay_t<decltype(grid)>;
    using ValueType = typename GridT::ValueType;
    auto &tree = grid.tree();

    BLI_assert(value.type().size == sizeof(ValueType));
    tree.root().setBackground(*static_cast<const ValueType *>(value.get()), true);
  });
}

}  // namespace blender::bke

#endif
