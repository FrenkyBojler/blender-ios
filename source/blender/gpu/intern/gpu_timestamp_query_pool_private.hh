/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

namespace blender::gpu {

/**
 * A class for recording timestamps on the GPU and matching them with CPU timestamps.
 */
class TimestampQueryPool {
 public:
  virtual ~TimestampQueryPool() = default;

  /* Resets the entire query pool. */
  virtual void reset() = 0;
  /* Write a timestamp for the specified query index. */
  virtual void write_timestamp(unsigned int index) = 0;
  /* Read the range of query indices. Blocks when the queries are not ready and resets the queries.
   */
  virtual void read_timestamps_sync(unsigned int index_first,
                                    unsigned int num_queries,
                                    uint64_t *timestamps_device) = 0;
  /* Converts a timestamp from the device domain to the host domain. */
  virtual uint64_t convert_timestamp_device_to_host_domain(uint64_t timestamp_device) = 0;
  /* Returns the current timestamp in the host domain (unit: nanoseconds). */
  virtual uint64_t get_host_timestamp_now() = 0;
};

}  // namespace blender::gpu
