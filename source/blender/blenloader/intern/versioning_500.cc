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

#include "BKE_attribute_legacy_convert.hh"
#include "BKE_colortools.hh"
#include "BKE_main.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

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

static void initialize_closure_input_structure_types(bNodeTree &ntree)
{
  LISTBASE_FOREACH (bNode *, node, &ntree.nodes) {
    if (node->type_legacy == GEO_NODE_EVALUATE_CLOSURE) {
      auto *storage = static_cast<NodeGeometryEvaluateClosure *>(node->storage);
      for (const int i : blender::IndexRange(storage->input_items.items_num)) {
        NodeGeometryEvaluateClosureInputItem &item = storage->input_items.items[i];
        if (item.structure_type == NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
          item.structure_type = NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_DYNAMIC;
        }
      }
      for (const int i : blender::IndexRange(storage->output_items.items_num)) {
        NodeGeometryEvaluateClosureOutputItem &item = storage->output_items.items[i];
        if (item.structure_type == NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO) {
          item.structure_type = NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_DYNAMIC;
        }
      }
    }
  }
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

    /* Change default Sky Texture to Nishita (after removal of old sky models) */
    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type == NTREE_SHADER) {
        LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
          if (node->type_legacy == SH_NODE_TEX_SKY && node->storage) {
            NodeTexSky *tex = (NodeTexSky *)node->storage;
            tex->sky_model = 0;
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 2)) {
    LISTBASE_FOREACH (PointCloud *, pointcloud, &bmain->pointclouds) {
      blender::bke::pointcloud_convert_customdata_to_storage(*pointcloud);
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 3)) {
    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type == NTREE_GEOMETRY) {
        initialize_closure_input_structure_types(*ntree);
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 7)) {
    const int uv_select_island = 1 << 3;
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      ToolSettings *ts = scene->toolsettings;
      if (ts->uv_selectmode & uv_select_island) {
        ts->uv_selectmode = UV_SELECT_VERTEX;
        ts->uv_flag |= UV_FLAG_ISLAND_SELECT;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 8)) {
    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type != NTREE_COMPOSIT) {
        continue;
      }
      LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
        if (node->type_legacy != CMP_NODE_DISPLACE) {
          continue;
        }
        if (node->storage != nullptr) {
          continue;
        }
        NodeDisplaceData *data = MEM_callocN<NodeDisplaceData>(__func__);
        data->interpolation = CMP_NODE_INTERPOLATION_ANISOTROPIC;
        node->storage = data;
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 9)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      if (STREQ(scene->r.engine, RE_engine_id_BLENDER_EEVEE_NEXT)) {
        STRNCPY(scene->r.engine, RE_engine_id_BLENDER_EEVEE);
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 10)) {
    LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
      LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
        view_layer->eevee.ambient_occlusion_distance = scene->eevee.gtao_distance;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 13)) {
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
