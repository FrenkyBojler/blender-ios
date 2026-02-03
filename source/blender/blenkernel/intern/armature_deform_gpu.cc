/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * \brief Helper functions for GPU Skinning
 */

#include "BKE_armature_deform_gpu.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_workspace.hh"

#include "BLI_listbase.h"

#include "DEG_depsgraph_query.hh"

#include "DNA_modifier_types.h"
#include "DNA_space_enums.h"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"

#include "WM_api.hh"

bool BKE_skinning_available_user()
{
  if (G.background) {
    return false;
  }
  if (GPU_backend_get_type() == GPU_BACKEND_NONE) {
    return false;
  }

  if (GPU_max_compute_shader_storage_blocks() < 8) {
    return false;
  }

  if ((U.gpu_flag & USER_GPU_FLAG_DEFORMATION_EVALUATION) == 0) {
    return false;
  }
  else {
    return true;
  }
}

// Getter function of any 3D viewport is in rendered mode
static bool BKE_is_any_viewport_rendered(const Depsgraph &depsgraph)
{
  Main *bmain = DEG_get_bmain(&depsgraph);

  if (!bmain || BLI_listbase_is_empty(&bmain->wm)) {
    return false;
  }

  LISTBASE_FOREACH (wmWindowManager *, wm, &bmain->wm) {

    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      bScreen *screen = BKE_workspace_active_screen_get(win->workspace_hook);
      if (!screen) {
        continue;
      }
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {

        if (area->spacetype == SPACE_VIEW3D) {
          View3D *v3d = static_cast<View3D *>(area->spacedata.first);

          if (v3d && v3d->shading.type == OB_RENDER) {
            return true;
          }
        }
      }
    }
  }

  return false;
}

static void BKE_skinning_cycles_warning(const Depsgraph &depsgraph)
{
  Main *bmain = DEG_get_bmain(&depsgraph);
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  ReportList *reports = &wm->runtime->reports;
  BKE_reportf(reports,
              RPT_WARNING,
              "GPU Deformations don't support Cycles Render, switching back to CPU Deforms");
  if (!G.background) {
    WM_report_banner_show(wm, nullptr);
  }
}

/* Getter function exists to disqualify Cycles from using GPU Skinning */
bool BKE_skinning_is_cycles_active(const Scene &scene, const Depsgraph &depsgraph)
{
  static bool cycles_report = false;

  if (STREQ(scene.r.engine, "CYCLES")) {

    if (DEG_get_mode(&depsgraph) == DAG_EVAL_RENDER || BKE_is_any_viewport_rendered(depsgraph)) {

      if (!cycles_report) {
        BKE_skinning_cycles_warning(depsgraph);
        cycles_report = true;
      }
      return true;
    }
    cycles_report = false;
  }
  return false;
}

static bool BKE_skinning_object_mode(const Object &ob)
{
  const int blocked_modes = OB_MODE_EDIT | OB_MODE_SCULPT | OB_MODE_VERTEX_PAINT |
                            OB_MODE_TEXTURE_PAINT | OB_MODE_WEIGHT_PAINT;

  return (ob.mode & blocked_modes) == 0;
}

bool BKE_is_skinning_possible(const Object &ob, const Scene &scene, const Depsgraph &depsgraph)
{
  if (!BKE_skinning_available_user() || !BKE_skinning_object_mode(ob) ||
      BKE_skinning_is_cycles_active(scene, depsgraph))
  {
    return false;
  }

  LISTBASE_FOREACH (ModifierData *, md, &ob.modifiers) {
    if (md->type == eModifierType_Armature) {
      if (md->mode & eModifierMode_Realtime) {
        return true;
      }
    }
  }

  return false;
}
