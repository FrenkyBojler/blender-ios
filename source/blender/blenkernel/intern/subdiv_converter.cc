/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "subdiv_converter.hh"

#include "opensubdiv_converter_capi.hh"

namespace blender::bke::subdiv {

void converter_free(OpenSubdiv_Converter *converter)
{
  if (converter->freeUserData) {
    converter->freeUserData(converter);
  }
}

int converter_vtx_boundary_interpolation_from_settings(const Settings *settings)
{
  switch (settings->vtx_boundary_interpolation) {
    case SubdivVtxBoundaryNone:
      return OSD_VTX_BOUNDARY_NONE;
    case SubdivVtxBoundaryEdgeOnly:
      return OSD_VTX_BOUNDARY_EDGE_ONLY;
    case SubdivVtxBoundaryEdgeAndCorner:
      return OSD_VTX_BOUNDARY_EDGE_AND_CORNER;
  }
  BLI_assert_msg(0, "Unknown vtx boundary interpolation");
  return OSD_VTX_BOUNDARY_EDGE_ONLY;
}

/*OpenSubdiv_FVarLinearInterpolation*/ int converter_fvar_linear_from_settings(
    const Settings *settings)
{
  switch (settings->fvar_linear_interpolation) {
    case SubdivFvarLinearInterpolationNone:
      return OSD_FVAR_LINEAR_INTERPOLATION_NONE;
    case SubdivFvarLinearInterpolationCornersOnly:
      return OSD_FVAR_LINEAR_INTERPOLATION_CORNERS_ONLY;
    case SubdivFvarLinearInterpolationCornersAndJunctions:
      return OSD_FVAR_LINEAR_INTERPOLATION_CORNERS_PLUS1;
    case SubdivFvarLinearInterpolationCornersJunctionsAndConcave:
      return OSD_FVAR_LINEAR_INTERPOLATION_CORNERS_PLUS2;
    case SubdivFvarLinearInterpolationBoundaries:
      return OSD_FVAR_LINEAR_INTERPOLATION_BOUNDARIES;
    case SubdivFvarLinearInterpolationAll:
      return OSD_FVAR_LINEAR_INTERPOLATION_ALL;
  }
  BLI_assert_msg(0, "Unknown fvar linear interpolation");
  return OSD_FVAR_LINEAR_INTERPOLATION_NONE;
}

}  // namespace blender::bke::subdiv
