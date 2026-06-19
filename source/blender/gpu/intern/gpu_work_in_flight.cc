/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "gpu_backend.hh"
#include "gpu_work_in_flight_private.hh"

namespace blender {

using namespace blender::gpu;

gpu::WorkInFlight *GPU_work_in_flight_create(unsigned int max_in_flight)
{
  WorkInFlight *work_in_flight = GPUBackend::get()->work_in_flight_alloc(max_in_flight);
  return work_in_flight;
}

void GPU_work_in_flight_free(gpu::WorkInFlight *work_in_flight)
{
  delete work_in_flight;
}

void GPU_work_in_flight_reset(gpu::WorkInFlight *work_in_flight)
{
  work_in_flight->reset();
}

void GPU_work_in_flight_begin_work(gpu::WorkInFlight *work_in_flight)
{
  work_in_flight->begin_work();
}

void GPU_work_in_flight_end_work(gpu::WorkInFlight *work_in_flight)
{
  work_in_flight->end_work();
}

}  // namespace blender
