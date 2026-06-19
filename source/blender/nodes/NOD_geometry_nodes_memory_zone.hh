/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_mutex.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_map.hh"

#include "BKE_node_socket_value.hh"

namespace blender::nodes {

struct MemoryZoneSignatureKey {
  int32_t output_identifier;
  Array<bke::SocketValueVariant> inputs;

  friend bool operator!=(const MemoryZoneSignatureKey &a, const MemoryZoneSignatureKey &b);
  uint64_t hash() const;
};

struct MemoryZoneValue {
  Array<std::optional<bke::SocketValueVariant>> output_values;
  void *zone_eval_data;
  Mutex mutex;
};

struct MemoryZonesCache {
  Mutex mutex;
  Map<MemoryZoneSignatureKey, MemoryZoneValue> caches;
};

}  // namespace blender::nodes
