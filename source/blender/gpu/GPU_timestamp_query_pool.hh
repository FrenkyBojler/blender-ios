/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <cstdint>

namespace blender {

namespace gpu {
class TimestampQueryPool;
}  // namespace gpu

/**
 * A class for recording timestamps on the GPU and matching them with CPU timestamps.
 */
gpu::TimestampQueryPool *GPU_timestamp_query_pool_create(unsigned int num_queries_max);
void GPU_timestamp_query_pool_free(gpu::TimestampQueryPool *timestamp_query_pool);

/* Resets the entire query pool. */
void GPU_timestamp_query_pool_reset(gpu::TimestampQueryPool *timestamp_query_pool);

/* Write a timestamp for the specified query index. */
void GPU_timestamp_query_pool_write(gpu::TimestampQueryPool *timestamp_query_pool,
                                    unsigned int index);

/* Read the range of query indices. Blocks when the queries are not ready and resets the queries.
 */
void GPU_timestamp_query_pool_read_sync(gpu::TimestampQueryPool *timestamp_query_pool,
                                        unsigned int index_first,
                                        unsigned int num_queries,
                                        uint64_t *timestamps_device);

/* Converts a timestamp from the device domain to the host domain. */
uint64_t GPU_timestamp_query_pool_convert_device_to_host_domain(
    gpu::TimestampQueryPool *timestamp_query_pool, uint64_t timestamp_device);

/* Returns the current timestamp in the host domain (unit: nanoseconds). */
uint64_t GPU_timestamp_query_pool_get_host_now(gpu::TimestampQueryPool *timestamp_query_pool);

}  // namespace blender
