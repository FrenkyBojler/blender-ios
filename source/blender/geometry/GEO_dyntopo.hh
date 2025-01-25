/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

struct Mesh;

namespace blender::geometry::dyntopo {

Mesh *subdivide(const Mesh &src_mesh,
                Span<float2> projection,
                float2 position,
                float radius,
                float max_length);

}  // namespace blender::geometry::dyntopo
