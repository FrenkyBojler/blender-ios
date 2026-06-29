/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "CLG_log.h"

#include "eevee_work_in_flight.hh"

#include "gpu_timestamp_query_pool.hh"

namespace blender::eevee {

static CLG_LogRef LOG = {"eevee"};

WorkInFlight::WorkInFlight(unsigned int num_in_flight_max, unsigned int num_in_flight_start)
    : num_in_flight_max_(num_in_flight_max), num_in_flight_start_(num_in_flight_start)
{
  query_pool_ = GPU_timestamp_query_pool_create(num_in_flight_max * 2);
  work_to_query_index_map_.resize(num_in_flight_start);
  work_to_query_index_map_.fill(-1);
}

WorkInFlight::~WorkInFlight()
{
  GPU_timestamp_query_pool_free(query_pool_);
}

void WorkInFlight::reset()
{
  GPU_timestamp_query_pool_reset(query_pool_);
  work_to_query_index_map_.resize(num_in_flight_start_);
  work_to_query_index_map_.fill(-1);
  work_index_ = 0;
}

void WorkInFlight::begin_work()
{
  int query_index = work_to_query_index_map_[work_index_];
  if (query_index >= 0) {
    uint64_t timestamps_begin_end[2];
    uint64_t time_before_wait = GPU_timestamp_query_pool_get_host_now(query_pool_);
    GPU_timestamp_query_pool_read_sync(
        query_pool_, uint32_t(query_index * 2), 2, timestamps_begin_end);
    uint64_t time_execute_start = GPU_timestamp_query_pool_convert_device_to_host_domain(
        query_pool_, timestamps_begin_end[0]);
    if (time_before_wait > time_execute_start &&
        uint32_t(work_to_query_index_map_.size()) < num_in_flight_max_)
    {
      /* Increase amount of work in flight. */
      work_to_query_index_map_.insert(work_index_, -1);
      std::string message = fmt::format("Increased work in flight to {}",
                                        work_to_query_index_map_.size());
      CLOG_INFO(&LOG, message.c_str());
    }
  }
  else {
    query_index = next_new_query_index++;
    work_to_query_index_map_[work_index_] = query_index;
  }
  GPU_timestamp_query_pool_write(query_pool_, uint32_t(query_index * 2));
}

void WorkInFlight::end_work()
{
  int query_index = work_to_query_index_map_[work_index_];
  GPU_timestamp_query_pool_write(query_pool_, uint32_t(query_index * 2 + 1));

  /* Metal: Perform render step between samples to allow flushing of freed GPUBackend resources.
   * Vulkan: Perform render step between samples to avoid allocation of a high amount of command
   * buffer memory that can eventually result in out-of-memory errors or a TDR when submitted as
   * one large command buffer. */
  if (ELEM(GPU_backend_get_type(), GPU_BACKEND_METAL, GPU_BACKEND_VULKAN)) {
    GPU_flush();
  }
  work_index_ = (work_index_ + 1) % work_to_query_index_map_.size();
}

}  // namespace blender::eevee
