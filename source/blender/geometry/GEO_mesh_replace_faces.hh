/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"
#include "FN_field.hh"

namespace blender {

struct Mesh;
struct PointCloud;

namespace geometry {

Mesh *replace_faces(const Mesh &base,
                    const fn::Field<bool> &selection_field,
                    const fn::Field<int> &indices_field,
                    Span<const Mesh *> meshes);

};  // namespace geometry

}  // namespace blender
