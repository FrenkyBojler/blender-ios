/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_context_private.hh"
#include "gpu_work_in_flight_private.hh"

namespace blender::gpu {
class TimestampQueryPool;
}  // namespace blender::gpu

namespace blender::eevee {

/**
 * Limiter for the maximum amount of work packets simultaneously in flight on the GPU.
 */
class WorkInFlight {
  uint32_t num_in_flight_max_, num_in_flight_start_;
  size_t work_index_ = 0;
  int next_new_query_index = 0;
  Vector<int> work_to_query_index_map_;
  gpu::TimestampQueryPool *query_pool_;

 public:
  WorkInFlight(unsigned int num_in_flight_max, unsigned int num_in_flight_start);
  ~WorkInFlight();

  /* Reset the internal state. Needs to be called before the first or after the last work packet.
   */
  void reset();

  /* Needs to be called before beginning a work packet. Blocks if the maximum number of work
   * packets in flight is reached. */
  void begin_work();

  /* Needs to be called after the end of a work packet. */
  void end_work();
};

}  // namespace blender::eevee
