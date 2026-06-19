/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_context_private.hh"
#include "gpu_work_in_flight_private.hh"

namespace blender::gpu {

class MTLWorkInFlight : public WorkInFlight {
 public:
  MTLWorkInFlight(unsigned int max_in_flight);
  ~MTLWorkInFlight() override = default;

  void reset() override;
  void begin_work() override;
  void end_work() override;
};

}  // namespace blender::gpu
