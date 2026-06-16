/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_mutex.hh"
#include "BLI_map.hh"

namespace blender::nodes {

struct MemoryZoneSignatureKey {
  int32_t output_identifier;
  Array<GMutablePointer> inputs;
  
  uint64_t hash() const;
  friend bool operator!=(const MemoryZoneSignatureKey &a, const MemoryZoneSignatureKey &b);
};

struct MemoryZoneValue {
  Array<GMutablePointer> output_values;
  void *zone_data;
  Mutex mutex;
};

struct MemoryZonesCache {
  Mutex mutex;
  Map<MemoryZoneSignatureKey, MemoryZoneValue> caches;
};

}  // namespace blender::nodes
