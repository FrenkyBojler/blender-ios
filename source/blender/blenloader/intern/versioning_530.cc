/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_scene_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

void do_versions_after_linking_530(FileData * /*fd*/, Main * /*bmain*/)
{
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

void blo_do_versions_530(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 1)) {
    for (Scene &scene : bmain->scenes) {
      VPaint *wpaint = scene.toolsettings->wpaint;
      if (wpaint) {
        const StringRefNull old_asset_id =
            wpaint->paint.brush_asset_reference->relative_asset_identifier;
        if (wpaint->paint.brush == nullptr && old_asset_id.endswith("Paint")) {
          /* The "Paint" brush asset was renamed to "Add Weight", find it via the default instead
           * of hardcoding the new name. */
          if (std::optional<AssetWeakReference> paint_brush_asset_reference =
                  BKE_paint_brush_type_default_reference(PaintMode::Weight,
                                                         WPAINT_BRUSH_TYPE_DRAW))
          {
            BKE_paint_brush_set(bmain, &wpaint->paint, *paint_brush_asset_reference);
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 2)) {
    for (Scene &scene : bmain->scenes) {
      if (scene.toolsettings) {
        if (scene.toolsettings->snap_exclude_active_edit_mode == 0) {
          scene.toolsettings->snap_exclude_active_edit_mode = (scene.toolsettings->snap_flag &
                                                               SCE_SNAP_UNUSED_4) ?
                                                                  eSnapMode(short(0xffff)) :
                                                                  SCE_SNAP_TO_NONE;
        }
        if (scene.toolsettings->snap_exclude_edited_edit_mode == 0) {
          scene.toolsettings->snap_exclude_edited_edit_mode = (scene.toolsettings->snap_flag &
                                                               SCE_SNAP_UNUSED_8) ?
                                                                  SCE_SNAP_TO_NONE :
                                                                  eSnapMode(short(0xffff));
        }
        if (scene.toolsettings->snap_exclude_non_edited_edit_mode == 0) {
          scene.toolsettings->snap_exclude_non_edited_edit_mode = (scene.toolsettings->snap_flag &
                                                                   SCE_SNAP_UNUSED_9) ?
                                                                      SCE_SNAP_TO_NONE :
                                                                      eSnapMode(short(0xffff));
        }
        if (scene.toolsettings->snap_exclude_non_selectable == 0) {
          scene.toolsettings->snap_exclude_non_selectable = (scene.toolsettings->snap_flag &
                                                             SCE_SNAP_UNUSED_10) ?
                                                                eSnapMode(short(0xffff)) :
                                                                SCE_SNAP_TO_NONE;
        }
        scene.toolsettings->snap_flag &= ~(SCE_SNAP_UNUSED_4 | SCE_SNAP_UNUSED_8 |
                                           SCE_SNAP_UNUSED_9 | SCE_SNAP_UNUSED_10);
      }
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
