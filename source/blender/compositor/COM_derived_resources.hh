/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_denoised_auxiliary_pass.hh"

namespace blender::realtime_compositor {

/* -------------------------------------------------------------------------------------------------
 * Derived Resources.
 *
 * Derived resources are resources that are derived from a particular result and stored in it.
 * Those resources can be needed by multiple operations, so caching them on the result will improve
 * performance at the cost of more memory usage. */
class DerivedResources {
 public:
  DenoisedAuxiliaryPassContainer denoised_auxiliary_passes;
};

}  // namespace blender::realtime_compositor
