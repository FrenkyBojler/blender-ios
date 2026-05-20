/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_bounds_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender {

struct Mesh;

namespace geometry {

/**
 * Calculates the bounds of a primitive ico_sphere.
 */

Bounds<float3> calculate_bounds_ico_sphere(const float radius, const int subdivisions);


Mesh *create_ico_sphere_mesh(const int subdivisions,
                                      const float radius,
                                      const std::optional<std::string> &uv_map_id);

}  // namespace geometry
}  // namespace blender
