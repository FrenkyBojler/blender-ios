/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "gpu_backend.hh"
#include "gpu_timestamp_query_pool_private.hh"

namespace blender {

using namespace blender::gpu;

gpu::TimestampQueryPool *GPU_timestamp_query_pool_create(unsigned int num_queries_max)
{
  TimestampQueryPool *timestamp_query_pool = GPUBackend::get()->timestamp_query_pool_alloc(
      num_queries_max);
  return timestamp_query_pool;
}

void GPU_timestamp_query_pool_free(gpu::TimestampQueryPool *timestamp_query_pool)
{
  delete timestamp_query_pool;
}

void GPU_timestamp_query_pool_reset(gpu::TimestampQueryPool *timestamp_query_pool)
{
  timestamp_query_pool->reset();
}

void GPU_timestamp_query_pool_write(gpu::TimestampQueryPool *timestamp_query_pool,
                                    unsigned int index)
{
  timestamp_query_pool->write_timestamp(index);
}

void GPU_timestamp_query_pool_read_sync(gpu::TimestampQueryPool *timestamp_query_pool,
                                        unsigned int index_first,
                                        unsigned int num_queries,
                                        uint64_t *timestamps_device)
{
  timestamp_query_pool->read_timestamps_sync(index_first, num_queries, timestamps_device);
}

uint64_t GPU_timestamp_query_pool_convert_device_to_host_domain(
    gpu::TimestampQueryPool *timestamp_query_pool, uint64_t timestamp_device)
{
  return timestamp_query_pool->convert_timestamp_device_to_host_domain(timestamp_device);
}

uint64_t GPU_timestamp_query_pool_get_host_now(gpu::TimestampQueryPool *timestamp_query_pool)
{
  return timestamp_query_pool->get_host_timestamp_now();
}

}  // namespace blender
