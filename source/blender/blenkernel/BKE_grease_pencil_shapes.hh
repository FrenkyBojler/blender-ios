/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <optional>

#include "BKE_grease_pencil.hh"

namespace blender::bke::greasepencil {

std::optional<ShapeCache> shape_cache_from_shape_ids(const int num_curves,
                                                     const VArray<int> &shape_ids);

}  // namespace blender::bke::greasepencil
