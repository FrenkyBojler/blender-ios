/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_context_private.hh"
#include "gpu_timestamp_query_pool_private.hh"

#include "mtl_context.hh"

namespace blender::gpu {

/* TODO:
 * - Apparently, Apple Silicon only supports MTLCounterSamplingPointAtStageBoundary.
 * - https://github.com/gfx-rs/wgpu/issues/9414 */

class MTLTimestampQueryPool : public TimestampQueryPool {
  unsigned int num_queries_max_ = 0;
  /* GPU counter sample buffer holding one timestamp per query index. */
  id<MTLCounterSampleBuffer> counter_sample_buffer_ = nil;
  /* Conversion factor between `mach_absolute_time` ticks and nanoseconds. */
  unsigned int timebase_numer_ = 1;
  unsigned int timebase_denom_ = 1;

  uint64_t mach_time_to_nanoseconds(uint64_t mach_time);

 public:
  MTLTimestampQueryPool(unsigned int num_queries_max);
  ~MTLTimestampQueryPool() override;

  void reset() override;
  void write_timestamp(unsigned int index) override;
  void read_timestamps_sync(unsigned int index_first,
                            unsigned int num_queries,
                            uint64_t *timestamps_device) override;
  uint64_t convert_timestamp_device_to_host_domain(uint64_t timestamp_device) override;
  uint64_t get_host_timestamp_now() override;
};

}  // namespace blender::gpu
