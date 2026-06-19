/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "mtl_work_in_flight.hh"

namespace blender::gpu {

MTLWorkInFlight::MTLWorkInFlight(unsigned int /*max_in_flight*/) {}

void MTLWorkInFlight::reset() {}

void MTLWorkInFlight::begin_work() {}

void MTLWorkInFlight::end_work()
{
  /* Perform render step between samples to allow flushing of freed GPUBackend resources. */
  GPU_flush();
}

}  // namespace blender::gpu
