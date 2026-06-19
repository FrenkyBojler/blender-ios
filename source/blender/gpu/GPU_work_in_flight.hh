/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

namespace blender {

namespace gpu {
class WorkInFlight;
}  // namespace gpu

gpu::WorkInFlight *GPU_work_in_flight_create(unsigned int max_in_flight);
void GPU_work_in_flight_free(gpu::WorkInFlight *work_in_flight);

void GPU_work_in_flight_reset(gpu::WorkInFlight *work_in_flight);
void GPU_work_in_flight_begin_work(gpu::WorkInFlight *work_in_flight);
void GPU_work_in_flight_end_work(gpu::WorkInFlight *work_in_flight);

}  // namespace blender
