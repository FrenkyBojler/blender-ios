/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_mesh_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "BKE_colortools.hh"
#include "BKE_main.hh"
#include "BKE_mesh_legacy_convert.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"
// static CLG_LogRef LOG = {"blo.readfile.doversion"};

static CustomDataLayer *find_old_seam_layer(CustomData &custom_data, const blender::StringRef name)
{
  for (CustomDataLayer &layer : blender::MutableSpan(custom_data.layers, custom_data.totlayer)) {
    if (layer.name == name) {
      return &layer;
    }
  }
  return nullptr;
}

static void rename_mesh_uv_seam_attribute(Mesh &mesh)
{
  using namespace blender;
  CustomDataLayer *old_seam_layer = find_old_seam_layer(mesh.edge_data, ".uv_seam");
  if (!old_seam_layer) {
    return;
  }
  Set<StringRef> names;
  for (const CustomDataLayer &layer : Span(mesh.vert_data.layers, mesh.vert_data.totlayer)) {
    if (layer.type & CD_MASK_PROP_ALL) {
      names.add(layer.name);
    }
  }
  for (const CustomDataLayer &layer : Span(mesh.edge_data.layers, mesh.edge_data.totlayer)) {
    if (layer.type & CD_MASK_PROP_ALL) {
      names.add(layer.name);
    }
  }
  for (const CustomDataLayer &layer : Span(mesh.face_data.layers, mesh.face_data.totlayer)) {
    if (layer.type & CD_MASK_PROP_ALL) {
      names.add(layer.name);
    }
  }
  for (const CustomDataLayer &layer : Span(mesh.corner_data.layers, mesh.corner_data.totlayer)) {
    if (layer.type & CD_MASK_PROP_ALL) {
      names.add(layer.name);
    }
  }
  LISTBASE_FOREACH (const bDeformGroup *, vertex_group, &mesh.vertex_group_names) {
    names.add(vertex_group->name);
  }

  /* If the new UV name is already taken, still rename the attribute so it becomes visible in the
   * list. Then the user can deal with the name conflict themselves. */
  const std::string new_name = BLI_uniquename_cb(
      [&](const StringRef name) { return names.contains(name); }, '.', "uv_seam");
  STRNCPY(old_seam_layer->name, new_name.c_str());
}

void do_versions_after_linking_500(FileData * /*fd*/, Main * /*bmain*/)
{
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

static void do_versions_apply_unified_paint_settings_to_all_modes(Scene &scene)
{
  const UnifiedPaintSettings &scene_ups = scene.toolsettings->unified_paint_settings;
  auto apply_to_paint = [&](Paint *paint) {
    if (paint == nullptr) {
      return;
    }
    UnifiedPaintSettings &ups = paint->unified_paint_settings;

    ups.size = scene_ups.size;
    ups.unprojected_radius = scene_ups.unprojected_radius;
    ups.alpha = scene_ups.alpha;
    ups.weight = scene_ups.weight;
    copy_v3_v3(ups.rgb, scene_ups.rgb);
    copy_v3_v3(ups.secondary_rgb, scene_ups.secondary_rgb);
    ups.color_jitter_flag = scene_ups.color_jitter_flag;
    copy_v3_v3(ups.hsv_jitter, scene_ups.hsv_jitter);

    BLI_assert(ups.curve_rand_hue == nullptr);
    BLI_assert(ups.curve_rand_saturation == nullptr);
    BLI_assert(ups.curve_rand_value == nullptr);
    ups.curve_rand_hue = BKE_curvemapping_copy(scene_ups.curve_rand_hue);
    ups.curve_rand_saturation = BKE_curvemapping_copy(scene_ups.curve_rand_saturation);
    ups.curve_rand_value = BKE_curvemapping_copy(scene_ups.curve_rand_value);
    ups.flag = scene_ups.flag;
  };

  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->vpaint));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->wpaint));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->sculpt));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->gp_paint));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->gp_vertexpaint));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->gp_sculptpaint));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->gp_weightpaint));
  apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->curves_sculpt));
  apply_to_paint(reinterpret_cast<Paint *>(&scene.toolsettings->imapaint));
}

void blo_do_versions_500(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  using namespace blender;
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 1)) {
    LISTBASE_FOREACH (Mesh *, mesh, &bmain->meshes) {
      bke::mesh_sculpt_mask_to_generic(*mesh);
      bke::mesh_custom_normals_to_generic(*mesh);
      rename_mesh_uv_seam_attribute(*mesh);
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 2)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      do_versions_apply_unified_paint_settings_to_all_modes(*scene);
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}
