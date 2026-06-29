/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <mach/mach_time.h>

#include "BLI_assert.hh"

#include "GPU_state.hh"

#include "mtl_backend.hh"
#include "mtl_context.hh"
#include "mtl_timestamp_query_pool.hh"

namespace blender::gpu {

/* Locate the timestamp counter set exposed by the device. Returns nil when the platform does not
 * support timestamp counters. */
static id<MTLCounterSet> find_timestamp_counter_set(id<MTLDevice> device)
{
  for (id<MTLCounterSet> counter_set in device.counterSets) {
    if ([counter_set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      return counter_set;
    }
  }
  return nil;
}

MTLTimestampQueryPool::MTLTimestampQueryPool(unsigned int num_queries_max)
    : num_queries_max_(num_queries_max)
{
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx);

  /* Cache the conversion factor between `mach_absolute_time` ticks and nanoseconds. On Apple
   * Silicon this is 1:1, but it can differ on Intel-based Macs. */
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  timebase_numer_ = timebase.numer;
  timebase_denom_ = timebase.denom;

  /* Allocate a counter sample buffer holding one timestamp per query index. */
  id<MTLCounterSet> timestamp_counter_set = find_timestamp_counter_set(ctx->device);
  BLI_assert_msg(timestamp_counter_set != nil,
                 "Timestamp counters are not supported on this Metal device.");

  MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
  descriptor.counterSet = timestamp_counter_set;
  descriptor.sampleCount = num_queries_max_;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.label = @"MTLTimestampQueryPool";

  NSError *error = nil;
  counter_sample_buffer_ = [ctx->device newCounterSampleBufferWithDescriptor:descriptor
                                                                       error:&error];
  [descriptor release];
  if (error != nil) {
    NSLog(@"Failed to create timestamp counter sample buffer: %@", error);
    BLI_assert_unreachable();
  }
}

MTLTimestampQueryPool::~MTLTimestampQueryPool()
{
  [counter_sample_buffer_ release];
  counter_sample_buffer_ = nil;
}

void MTLTimestampQueryPool::reset()
{
  /* No explicit reset necessary. Sampling a query index overwrites the previous value. */
}

void MTLTimestampQueryPool::write_timestamp(unsigned int index)
{
  BLI_assert(index < num_queries_max_);
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx);

  /* TODO */
}

void MTLTimestampQueryPool::read_timestamps_sync(unsigned int index_first,
                                                 unsigned int num_queries,
                                                 uint64_t *timestamps_device)
{
  /* TODO */
}

uint64_t MTLTimestampQueryPool::convert_timestamp_device_to_host_domain(uint64_t timestamp_device)
{
  /* Sample a correlated pair of CPU and GPU timestamps to align the two clock domains. */
  MTLTimestamp calib_timestamp_host = 0;
  MTLTimestamp calib_timestamp_device = 0;
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx);
  [ctx->device sampleTimestamps:&calib_timestamp_host gpuTimestamp:&calib_timestamp_device];

  /* GPU timestamps are reported in nanoseconds, so the offset from the calibration sample can be
   * applied directly. */
  int64_t time_offset_device = int64_t(timestamp_device) - int64_t(calib_timestamp_device);
  int64_t timestamp_converted = int64_t(mach_time_to_nanoseconds(calib_timestamp_host)) +
                                time_offset_device;
  if (timestamp_converted < 0) {
    timestamp_converted = 0;
  }
  return uint64_t(timestamp_converted);
}

uint64_t MTLTimestampQueryPool::get_host_timestamp_now()
{
  return mach_time_to_nanoseconds(mach_absolute_time());
}

uint64_t MTLTimestampQueryPool::mach_time_to_nanoseconds(uint64_t mach_time)
{
  /* Avoid overflow while keeping full precision when the timebase is a clean ratio. */
  if (timebase_numer_ == timebase_denom_) {
    return mach_time;
  }
  return uint64_t((double(mach_time) * double(timebase_numer_)) / double(timebase_denom_));
}

}  // namespace blender::gpu
