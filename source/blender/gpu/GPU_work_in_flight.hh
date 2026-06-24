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

/**
 * Limiter for the maximum amount of work packets simultaneously in flight on the GPU.
 */
gpu::WorkInFlight *GPU_work_in_flight_create(unsigned int max_in_flight);
void GPU_work_in_flight_free(gpu::WorkInFlight *work_in_flight);

/* Reset the internal state. Needs to be called before the first or after the last work packet. */
void GPU_work_in_flight_reset(gpu::WorkInFlight *work_in_flight);

/* Needs to be called before beginning a work packet. Blocks if the maximum number of work packets
 * in flight is reached. */
void GPU_work_in_flight_begin_work(gpu::WorkInFlight *work_in_flight);

/* Needs to be called after the end of a work packet. */
void GPU_work_in_flight_end_work(gpu::WorkInFlight *work_in_flight);

}  // namespace blender
