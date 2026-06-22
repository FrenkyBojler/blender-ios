/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

namespace blender::gpu {

class WorkInFlight {
 public:
  virtual ~WorkInFlight() = default;
  virtual void reset() = 0;
  virtual void begin_work() = 0;
  virtual void end_work() = 0;
};

}  // namespace blender::gpu
