/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_timestamp_query_pool_private.hh"

#include <epoxy/gl.h>

namespace blender::gpu {

class GLTimestampQueryPool : public TimestampQueryPool {
 private:
  unsigned int num_queries_max_;
  GLuint *query_ids_;

 public:
  GLTimestampQueryPool(unsigned int num_queries_max);
  ~GLTimestampQueryPool() override;

  void reset() override;
  void write_timestamp(unsigned int index) override;
  void read_timestamps_sync(unsigned int index_first,
                            unsigned int num_queries,
                            uint64_t *timestamps_device) override;
  uint64_t convert_timestamp_device_to_host_domain(uint64_t timestamp_device) override;
  uint64_t get_host_timestamp_now() override;
};

}  // namespace blender::gpu
