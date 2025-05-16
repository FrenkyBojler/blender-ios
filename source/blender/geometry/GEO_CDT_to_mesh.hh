/* SPDX-FileCopyrightText: 202 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_delaunay_2d.hh"

#include "BKE_mesh.hh"

namespace blender::geometry {

Mesh *cdts_to_mesh(const Span<meshintersect::CDT_result<double>> results);

}  // namespace blender::geometry
