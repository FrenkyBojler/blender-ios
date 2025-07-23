/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_volume_grid_fwd.hh"

#include "BLI_color.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"

#include "FN_field.hh"

#include "NOD_geometry_nodes_bundle_fwd.hh"
#include "NOD_geometry_nodes_closure_fwd.hh"

namespace blender::nodes {

template<typename T>
static constexpr bool geo_nodes_is_field_base_type_v = is_same_any_v<T,
                                                                     float,
                                                                     int,
                                                                     bool,
                                                                     ColorGeometry4f,
                                                                     float3,
                                                                     std::string,
                                                                     math::Quaternion,
                                                                     float4x4>;

template<typename T>
static constexpr bool geo_nodes_type_stored_as_SocketValueVariant_v =
    std::is_enum_v<T> || geo_nodes_is_field_base_type_v<T> || fn::is_field_v<T> ||
    bke::is_VolumeGrid_v<T> ||
    is_same_any_v<T, fn::GField, bke::GVolumeGrid, nodes::BundlePtr, nodes::ClosurePtr>;

}  // namespace blender::nodes
