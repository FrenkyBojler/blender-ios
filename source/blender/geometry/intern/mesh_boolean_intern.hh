/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_attribute.hh"

#include "BLI_span.hh"

struct Mesh;

namespace blender::geometry::boolean {

void interpolate_corner_attributes(bke::MutableAttributeAccessor output_attrs,
                                   bke::AttributeAccessor input_attrs,
                                   Mesh *output_mesh,
                                   const Mesh *input_mesh,
                                   Span<int> out_to_in_corner_map,
                                   Span<int> out_to_in_face_map);

}  // namespace blender::geometry::boolean
