/* SPDX-FileCopyrightText: 2025 Blender Authors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include <stdint.h>

/** \file
 * \ingroup bke
 */

namespace blender::bke {
constexpr uint8_t BONE_AXIS_MAIN = 0;       // not yet used
constexpr uint8_t BONE_AXIS_SECONDARY = 2;  // not yet used
constexpr uint8_t BONE_AXIS_ROLL = 2;

/**
 * Return the unit vector pointing from the bone's head to its tail.
 *
 * \param dimension is the coordinate axis index, X=0, Y=1, Z=2.
 */
constexpr float bone_axis_vector_roll(const uint8_t dimension)
{
  return dimension == BONE_AXIS_ROLL ? 1.0f : 0.0f;
}

constexpr float4x4::vec3_type &bone_axis_roll(float4x4 &matrix)
{
  switch (BONE_AXIS_ROLL) {
    case 0:
      return matrix.x_axis();
    case 1:
      return matrix.y_axis();
    case 2:
      return matrix.z_axis();
  }
}

}  // namespace blender::bke
