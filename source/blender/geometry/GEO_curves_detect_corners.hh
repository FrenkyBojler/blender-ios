/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_curves.hh"

namespace blender::geometry {

/**
 * Detects corners on the given poly curves and returns a boolean array with a selection.
 * TODO: params
 */
Array<bool> curves_detect_corners(const bke::CurvesGeometry &curves,
                                  const VArray<float> &angle_min,
                                  const VArray<float> &radius_min,
                                  const VArray<float> &radius_max,
                                  const VArray<int> &samples_max);

}  // namespace blender::geometry
