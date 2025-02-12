/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edpointcloud
 */

#include "BLI_offset_indices.hh"
#include "BLI_rand.hh"

#include "BKE_attribute.hh"

#include "DNA_pointcloud_types.h"

#include "ED_point_cloud.hh"

namespace blender::ed::point_cloud {

IndexMask random_mask(const PointCloud &point_cloud,
                      const IndexMask &mask,
                      const uint32_t random_seed,
                      const float probability,
                      IndexMaskMemory &memory)
{
  RandomNumberGenerator rng{random_seed};
  const auto next_bool_random_value = [&]() { return rng.get_float() <= probability; };

  const int64_t domain_size = point_cloud.attributes().domain_size(
      blender::bke::AttrDomain::Point);

  Array<bool> random(domain_size, false);
  mask.foreach_index_optimized<int64_t>(
      [&](const int64_t i) { random[i] = next_bool_random_value(); });

  return IndexMask::from_bools(IndexRange(domain_size), random, memory);
}

IndexMask random_mask(const PointCloud &point_cloud,
                      const uint32_t random_seed,
                      const float probability,
                      IndexMaskMemory &memory)
{
  const IndexRange selection(
      point_cloud.attributes().domain_size(blender::bke::AttrDomain::Point));
  return random_mask(point_cloud, selection, random_seed, probability, memory);
}

}  // namespace blender::ed::point_cloud
