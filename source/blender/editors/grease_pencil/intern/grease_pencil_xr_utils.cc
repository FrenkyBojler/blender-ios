/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BKE_context.hh"

#include "DNA_view3d_types.h"

#include "ED_grease_pencil.hh"

float ED_grease_pencil_pixfactor_calculate(bContext *C)
{
  float _pixfactor = 1.0f;
  if (!C || C == nullptr)
    return _pixfactor;
  View3D *v3d = CTX_wm_view3d(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmXrData *xr_data = &wm->xr;
  if ((v3d->flag & (V3D_XR_SESSION_SURFACE | V3D_XR_SESSION_MIRROR)) != 0) {
    float _tmp;
    WM_xr_session_state_nav_scale_get(xr_data, &_tmp);
    _pixfactor = 1.0 / _tmp;
  }

  /**
   * Convert to legacy "pixel" space. We divide here, because the shader expects the values to
   * be in the `px` space rather than world space. Otherwise the values will get clamped.
   *
   * The following line should be applied after, but since not every single place in the code
   * calculates _pixfactor with the legacy radius conversion in the same way, we need to let the
   * logic out of this helper function
   *  _pixfactor /= bke::greasepencil::LEGACY_RADIUS_CONVERSION_FACTOR;
   */
  return _pixfactor;
}
