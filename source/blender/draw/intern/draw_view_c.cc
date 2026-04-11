/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Contains dynamic drawing using immediate mode
 */

#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "GPU_debug.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "UI_view2d.hh"

#include "WM_types.hh"

#include "BKE_paint.hh"
#include "BKE_screen.hh"

#include "DRW_engine.hh"
#include "DRW_render.hh"

#include "draw_view_c.hh"

#include "view3d_intern.hh"

namespace blender {

/* ******************** region info ***************** */

void DRW_draw_region_info(const bContext *C, ARegion *region)
{
  GPU_debug_group_begin("RegionInfo");
  view3d_draw_region_info(C, region);
  GPU_debug_group_end();
}

/* **************************** 3D Gizmo ******************************** */

void DRW_draw_gizmo_3d(const bContext *C, ARegion *region)
{
  /* draw depth culled gizmos - gizmos need to be updated *after* view matrix was set up */
  /* TODO: depth culling gizmos is not yet supported, just drawing _3D here, should
   * later become _IN_SCENE (and draw _3D separate) */

  /* Temporarily neutralise ortho-stretch so transform gizmos (move/rotate/scale handles)
   * are not distorted by it. We save/restore winmat and persmat around the draw call. */
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  const float saved_viewscale_x = rv3d->viewscale_x;
  const float saved_viewscale_y = rv3d->viewscale_y;
  const bool has_stretch = (rv3d->viewscale_x != 0.0f) || (rv3d->viewscale_y != 0.0f);

  if (has_stretch) {
    float saved_winmat[4][4];
    float saved_persmat[4][4];
    float saved_persinv[4][4];
    copy_m4_m4(saved_winmat, rv3d->winmat);
    copy_m4_m4(saved_persmat, rv3d->persmat);
    copy_m4_m4(saved_persinv, rv3d->persinv);

    /* Undo the stretch from winmat so gizmos are projected without distortion.
     *
     * The viewscale expands the ortho viewplane width/height by (1 + viewscale_*).
     * This divides both the scale elements [0][0] / [1][1] AND the translation
     * offsets [3][0] / [3][1] of the projection matrix by the same factor.
     * Multiplying those four specific elements back by the scale recovers the
     * original unscaled projection. */
    float gizmo_winmat[4][4];
    copy_m4_m4(gizmo_winmat, rv3d->winmat);
    const float scale_x = 1.0f + saved_viewscale_x;
    const float scale_y = 1.0f + saved_viewscale_y;
    gizmo_winmat[0][0] *= scale_x; /* X scale coefficient */
    gizmo_winmat[3][0] *= scale_x; /* X translation offset (column 3, row 0) */
    gizmo_winmat[1][1] *= scale_y; /* Y scale coefficient */
    gizmo_winmat[3][1] *= scale_y; /* Y translation offset (column 3, row 1) */

    copy_m4_m4(rv3d->winmat, gizmo_winmat);
    mul_m4_m4m4(rv3d->persmat, rv3d->winmat, rv3d->viewmat);
    invert_m4_m4(rv3d->persinv, rv3d->persmat);
    GPU_matrix_projection_set(rv3d->winmat);

    /* Recompute pixsize from the corrected persmat so that draw_prepare computes the right
     * gz->scale_final. Without this, gizmos appear larger when the viewport is stretched because
     * pixsize was derived from the stretched (smaller-magnitude) projection columns. */
    const float saved_pixsize = rv3d->pixsize;
    {
      const float v1[3] = {rv3d->persmat[0][0], rv3d->persmat[1][0], rv3d->persmat[2][0]};
      const float v2[3] = {rv3d->persmat[0][1], rv3d->persmat[1][1], rv3d->persmat[2][1]};
      const float len_px = 2.0f / sqrtf(min_ff(len_squared_v3(v1), len_squared_v3(v2)));
      rv3d->pixsize = len_px / float(max_ii(region->winx, region->winy));
    }

    WM_gizmomap_draw(region->runtime->gizmo_map, C, WM_GIZMOMAP_DRAWSTEP_3D);

    rv3d->pixsize = saved_pixsize;
    copy_m4_m4(rv3d->winmat, saved_winmat);
    copy_m4_m4(rv3d->persmat, saved_persmat);
    copy_m4_m4(rv3d->persinv, saved_persinv);
    GPU_matrix_projection_set(rv3d->winmat);
  }
  else {
    WM_gizmomap_draw(region->runtime->gizmo_map, C, WM_GIZMOMAP_DRAWSTEP_3D);
  }
}

void DRW_draw_gizmo_2d(const bContext *C, ARegion *region)
{
  WM_gizmomap_draw(region->runtime->gizmo_map, C, WM_GIZMOMAP_DRAWSTEP_2D);
  GPU_depth_mask(true);
}

}  // namespace blender
