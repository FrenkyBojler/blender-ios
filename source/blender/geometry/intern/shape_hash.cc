/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <xxhash.h>

#include "BKE_geometry_set.hh"

#include "DNA_mesh_types.h"
#include "GEO_shape_hash.hh"

namespace blender::geometry::collision_shapes {

GeometryShapeHash from_geometry(const bke::GeometrySet &geometry_set)
{
  XXH3_state_t *hash_state = XXH3_createState();
  XXH3_128bits_reset(hash_state);

  if (const Mesh *mesh = geometry_set.get_mesh()) {
    const Span<float3> positions = mesh->vert_positions();
    XXH3_128bits_update(hash_state, positions.data(), positions.size_in_bytes());
  }

  const XXH128_hash_t digest = XXH3_128bits_digest(hash_state);
  XXH3_freeState(hash_state);

  GeometryShapeHash final_hash;
  static_assert(sizeof(final_hash) == sizeof(digest));
  memcpy(&final_hash, &digest, sizeof(digest));
  return final_hash;
}

}  // namespace blender::geometry::collision_shapes
